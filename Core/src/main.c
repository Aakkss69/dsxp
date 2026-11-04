/* Entry point: argument handling, wiring, and the 60 Hz service loop. */
#include "dsxp.h"
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static bool g_verify = false, g_ui = false, g_no_open = false;
static IOHIDManagerRef g_mgr;

static void on_signal(int s) { (void)s; g_stop = 1; }

static void resolve_dir(void) {
    char exe[PATH_MAX];
    if (proc_pidpath(getpid(), exe, sizeof exe) > 0) {
        strncpy(g_dir, exe, sizeof g_dir - 1);
        char *slash = strrchr(g_dir, '/');
        if (slash) *slash = 0;
    } else if (!getcwd(g_dir, sizeof g_dir)) {
        strcpy(g_dir, ".");
    }
}

static void verify_tick(void) {
    if (!g_have_report) { fprintf(stderr, "\rwaiting for report 0x31..."); return; }
    printf("\033[H\033[J");
    printf("dsxp --verify   (reports: %ld, deadzone %.3f, curve %.2f)\n\n",
           g_reports, g_tune.deadzone, g_tune.curve);
    for (int i = 0; i < S_COUNT; i++)
        printf("  %-16s %+.3f\n", SRC_LABEL[i], g_state.val[i]);
    printf("\n  Pressed:");
    bool any = false;
    for (int i = 0; i < B_COUNT; i++)
        if (g_state.btn[i]) { printf(" [%s]", BTN_NAME[i]); any = true; }
    if (!any) printf(" (none)");
    printf("\n\n  Ctrl-C to quit.\n");
    fflush(stdout);
}

static void timer_cb(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    if (g_stop) { CFRunLoopStop(CFRunLoopGetCurrent()); return; }
    reports_tick();
    wind_tick();
    light_tick();
    out_tick();
    if (g_verify) { verify_tick(); return; }

    static double last_gate = 0;
    static bool gate_ok = false;
    double now = now_sec();
    if (now - last_gate >= GATE_POLL_SEC) {
        last_gate = now;
        gate_rescan_transports(g_mgr);
        g_xp_running = gate_xplane_running();
        bool ok = g_bt_present && !g_usb_present && g_xp_running;
        if (ok != gate_ok) {
            fprintf(stderr, "[dsxp] gate: bluetooth=%d usb=%d xplane12=%d -> %s\n",
                    g_bt_present, g_usb_present, g_xp_running, ok ? "ACTIVE" : "INACTIVE");
            gate_ok = ok;
            if (ok) xp_engage(); else xp_disengage();
        }
    }
    if (gate_ok && g_engaged) xp_push();
}

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [--ui|--run|--verify] [-v] [--no-open]\n"
        "  --ui       bridge plus mapper UI on http://127.0.0.1:%d\n"
        "  --run      bridge only, headless\n"
        "  --verify   decode and print live input, send nothing to X-Plane\n"
        "  -v         log every command as it fires\n"
        "  --no-open  with --ui, do not launch a browser\n", p, UI_PORT);
}

int main(int argc, char **argv) {
    resolve_dir();

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--verify"))  g_verify = true;
        else if (!strcmp(argv[i], "--ui"))      g_ui = true;
        else if (!strcmp(argv[i], "--no-open")) g_no_open = true;
        else if (!strcmp(argv[i], "-v"))        g_verbose = true;
        else if (!strcmp(argv[i], "--run"))     { /* default mode */ }
        else { usage(argv[0]); return 2; }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    /* closing the terminal must also stop cleanly, or a latched rumble outlives us */
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);      /* a closed browser tab must not kill us */

    if (!g_verify) xp_udp_open();
    bindings_load();

    pthread_t th;
    if (g_ui) {
        commands_load();
        pthread_create(&th, NULL, httpd_thread, NULL);
    }

    g_mgr = ds_open();
    if (!g_mgr) return 1;

    if (g_ui && !g_no_open) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "open http://127.0.0.1:%d >/dev/null 2>&1", UI_PORT);
        if (system(cmd) != 0)
            fprintf(stderr, "[dsxp] open http://127.0.0.1:%d\n", UI_PORT);
    }

    CFRunLoopTimerContext tc = {0};
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(), 1.0 / 60.0, 0, 0, timer_cb, &tc);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRunLoopRun();

    xp_disengage();
    out_quiet();
    fprintf(stderr, "\n[dsxp] stopped. %ld reports processed.\n", g_reports);
    return 0;
}
