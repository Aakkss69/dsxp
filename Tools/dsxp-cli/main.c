/*
 * dsxp-cli - run the DSXP core headless, for hardware checks without the app.
 *
 *   dsxp-cli               bridge, prints a status line twice a second
 *   dsxp-cli --verify      decode and show live input; never talks to X-Plane
 *   dsxp-cli --capture N   print N raw Bluetooth 0x31 reports as hex (test fixtures)
 *   options: --data DIR  --logs DIR  --commands FILE  --run-for SEC  -v
 *
 * It uses the same settings folder as the app, so it refuses to run while the
 * app (or another dsxp-cli) holds the instance lock.
 */
#include "dsxp_core.h"
#include "core_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* written by the signal handler and the core thread, read by main (F26) */
static atomic_bool g_stop;
static atomic_long g_capture_left;
static bool g_verify;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ds_state_t g_view;
static gesture_t g_gest;
static tuning_t g_tune = { 0.06, 1.0, 0.5 };

static void on_signal(int s) { (void)s; atomic_store(&g_stop, true); }   /* lock-free: signal-safe */

static void raw_cb(void *ctx, const unsigned char *r, long len) {
    (void)ctx;
    if (atomic_load(&g_capture_left) > 0) {
        for (long i = 0; i < len; i++) printf("%02x%s", r[i], i + 1 < len ? " " : "\n");
        fflush(stdout);
        if (atomic_fetch_sub(&g_capture_left, 1) == 1) atomic_store(&g_stop, true);
    }
    if (g_verify) {
        pthread_mutex_lock(&g_lock);
        ds_decode_report(&g_view, &g_gest, &g_tune, r, len, now_sec());
        pthread_mutex_unlock(&g_lock);
    }
}

static void usage(const char *p) {
    fprintf(stderr, "usage: %s [--verify | --capture N] [--data DIR] [--logs DIR] "
                    "[--commands FILE] [--run-for SEC] [-v]\n", p);
}

static const char *link_name(int l) {
    switch (l) {
    case DSXP_LINK_BT: return "bluetooth";
    case DSXP_LINK_USB: return "usb";
    case DSXP_LINK_BT_AND_USB: return "bt+usb";
    default: return "none";
    }
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char data[1024], logs[1024], cmds[1100];
    snprintf(data, sizeof data, "%s/Library/Application Support/DSXP", home);
    snprintf(logs, sizeof logs, "%s/Library/Logs/DSXP", home);
    snprintf(cmds, sizeof cmds, "%s/X-Plane/X-Plane 12/Resources/plugins/Commands.txt", home);
    bool verbose = false;
    double run_for = 0;   /* seconds; 0 = until Ctrl-C */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verify")) g_verify = true;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            /* a typo must not silently turn a passive capture into live driving */
            char *e; errno = 0;
            long n = strtol(argv[++i], &e, 10);
            if (*e || e == argv[i] || errno || n < 1 || n > 1000000) { usage(argv[0]); return 2; }
            atomic_store(&g_capture_left, n);
        }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(data, sizeof data, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--logs") && i + 1 < argc) snprintf(logs, sizeof logs, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--commands") && i + 1 < argc) snprintf(cmds, sizeof cmds, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--run-for") && i + 1 < argc) {
            char *e; errno = 0;
            run_for = strtod(argv[++i], &e);
            if (*e || e == argv[i] || errno || !(run_for > 0)) { usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else { usage(argv[0]); return 2; }
    }

    mkdir_p(data, 0700);
    char lockp[1100];
    snprintf(lockp, sizeof lockp, "%s/instance.lock", data);
    int lk = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk >= 0 && flock(lk, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "DSXP is already running (the app or another dsxp-cli). Quit it first.\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    dsxp_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.data_dir = data;
    cfg.log_dir = logs;
    cfg.commands_path = access(cmds, R_OK) == 0 ? cmds : "";
    cfg.log_retention_days = 14;
    long capture = atomic_load(&g_capture_left);
    cfg.verbose = verbose || (!g_verify && capture == 0);
    cfg.dry_run = g_verify || capture > 0;
    cfg.on_raw_report = raw_cb;
    if (dsxp_start(&cfg) != 0) { fprintf(stderr, "dsxp_start failed\n"); return 1; }

    double t0 = now_sec();
    while (!atomic_load(&g_stop)) {
        usleep(g_verify ? 100000 : 500000);
        if (run_for > 0 && now_sec() - t0 >= run_for) break;
        if (atomic_load(&g_capture_left) > 0) continue;
        dsxp_status_t st;
        dsxp_get_status(&st);
        if (g_verify) {
            pthread_mutex_lock(&g_lock);
            ds_state_t v = g_view;
            pthread_mutex_unlock(&g_lock);
            printf("\033[H\033[Jdsxp-cli --verify   link=%s  %.1f Hz  (sends nothing to X-Plane)\n\n",
                   link_name(st.link), st.report_hz);
            for (int i = 0; i < S_COUNT; i++) printf("  %-16s %+.3f\n", SRC_LABEL[i], v.val[i]);
            printf("\n  Pressed:");
            bool any = false;
            for (int i = 0; i < B_COUNT; i++) if (v.btn[i]) { printf(" [%s]", BTN_NAME[i]); any = true; }
            printf("%s\n\n  Ctrl-C to quit.\n", any ? "" : " (none)");
        } else {
            printf("\rlink=%-9s input=%d %5.1f Hz  xplane=%s  engaged=%d  commands=%d   ",
                   link_name(st.link), st.input_live, st.report_hz,
                   !st.xp_running ? "off" : st.xp_answering ? "answering" : "running",
                   st.engaged, st.ncmds);
        }
        fflush(stdout);
    }
    printf("\n");
    dsxp_stop(2.0);
    return 0;
}
