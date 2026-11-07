/*
 * Crash guard.
 *
 * The controller keeps playing the last output report it received (rumble,
 * trigger resistance), and X-Plane keeps the override datarefs set, if the app
 * dies without cleaning up - a crash, kill -9, or Xcode's Stop button. The app
 * therefore starts a tiny helper: itself, re-executed with --dsxp-guard, holding
 * the read end of a pipe and a copy of the core's UDP socket.
 *
 * Pipe protocol (parent -> guard): 'Q' = clean quit, leave quietly;
 * 'P' hi lo = X-Plane's port changed. End of file without 'Q' means the parent
 * died: the guard releases X-Plane's controls, cancels the telemetry
 * subscriptions (from the same socket, so X-Plane matches them), quiets the
 * controller and exits.
 */
#include "core_internal.h"
#include "hid.h"             /* the DualSense through IOKit, shared with core_thread.c */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

int dsxp_guard_spawn(const char *exe_path) {
    if (!exe_path || !*exe_path) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    /* close-on-exec at once, so no other fork/exec can inherit the write end and
     * keep the pipe open after a crash (F24) */
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    /* copies at 10 and above, so neither can collide with the 3 and 4 they are
     * duplicated onto in the child */
    int rd = fcntl(fds[0], F_DUPFD_CLOEXEC, 10);
    close(fds[0]);
    int sock = runtime_udp_socket();
    int sk = sock >= 0 ? fcntl(sock, F_DUPFD_CLOEXEC, 10) : -1;
    if (rd < 0) { close(fds[1]); if (sk >= 0) close(sk); return -1; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, rd, 3);
    if (sk >= 0) posix_spawn_file_actions_adddup2(&fa, sk, 4);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* only fds 3 and 4 survive; own process group, so a Ctrl-C aimed at the app's
     * terminal does not take the guard down; signals the app ignores start at
     * their defaults again and none is blocked (F23) */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP |
                                    POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup(&attr, 0);
    sigset_t def, none;
    sigemptyset(&def);
    sigaddset(&def, SIGTERM); sigaddset(&def, SIGINT); sigaddset(&def, SIGHUP); sigaddset(&def, SIGPIPE);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setsigmask(&attr, &none);

    char port[16];
    snprintf(port, sizeof port, "%d", runtime_xp_port());
    char *argv[] = { (char *)exe_path, "--dsxp-guard", "3", sk >= 0 ? "4" : "-1", port, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, exe_path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(rd);
    if (sk >= 0) close(sk);
    if (rc != 0) { close(fds[1]); return -1; }
    return fds[1];
}

static void write_all(int fd, const unsigned char *b, size_t n) {
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        b += w;
        n -= (size_t)w;
    }
}

void dsxp_guard_release(int fd) {
    if (fd < 0) return;
    unsigned char q = 'Q';
    write_all(fd, &q, 1);
    close(fd);
}

void dsxp_guard_port(int fd, int port) {
    if (fd < 0 || port < 1 || port > 65535) return;
    unsigned char m[3] = { 'P', (unsigned char)(port >> 8), (unsigned char)(port & 0xFF) };
    write_all(fd, m, sizeof m);              /* 3 bytes < PIPE_BUF: one atomic write */
}

/* Everything the guard sends to X-Plane, in order. Pure, for tests. */
void guard_build_release(void (*send)(void *ctx, const unsigned char *buf, size_t len), void *ctx) {
    static const char *zero[] = {
        "sim/joystick/yoke_pitch_ratio",
        "sim/joystick/yoke_roll_ratio",
        "sim/joystick/yoke_heading_ratio",
        "sim/cockpit2/controls/left_brake_ratio",
        "sim/cockpit2/controls/right_brake_ratio",
        "sim/operation/override/override_joystick_pitch",
        "sim/operation/override/override_joystick_roll",
        "sim/operation/override/override_joystick_heading",
        "sim/operation/override/override_toe_brakes",
    };
    for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++) {
        unsigned char p[DREF_LEN];
        send(ctx, p, xp_build_dref(p, zero[i], 0.0f));
    }
    for (int id = 1; id <= telemetry_rref_count(); id++) {
        char path[128];
        if (!telemetry_rref_path(id, path, sizeof path)) continue;
        unsigned char p[RREF_LEN];
        send(ctx, p, xp_build_rref(p, 0, id, path));     /* frequency 0 cancels */
    }
}

typedef struct { int sock; struct sockaddr_in to; } guard_udp_t;

static void guard_send(void *ctx, const unsigned char *buf, size_t len) {
    guard_udp_t *g = ctx;
    sendto(g->sock, buf, len, 0, (const struct sockaddr *)&g->to, sizeof g->to);
}

static void release_xplane(int sock, int port) {
    guard_udp_t g;
    g.sock = sock >= 0 ? sock : socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sock < 0) return;
    memset(&g.to, 0, sizeof g.to);
    g.to.sin_family = AF_INET;
    g.to.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, XP_IP, &g.to.sin_addr);
    guard_build_release(guard_send, &g);
    if (sock < 0) close(g.sock);
}

/* hid_each_device: send a Bluetooth DualSense the all-off report (no rumble, no trigger
 * effect, no light), so it is not left buzzing when DSXP died. */
static bool quiet_one(IOHIDDeviceRef d, void *arg) {
    (void)arg;
    char tb[64];
    if (!hid_transport(d, tb, sizeof tb) || strcmp(tb, "Bluetooth") != 0) return true;
    out_frame_t f;
    memset(&f, 0, sizeof f);                         /* everything off                 */
    unsigned char rep[OUT_REPORT_LEN];
    out_build_report(&f, 0, rep);
    IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, BT_REPORT_ID, rep, sizeof rep);
    return true;                                     /* every one of them              */
}

static void quiet_controller(void) {
    IOHIDManagerRef mgr = hid_dualsense_manager();   /* platform/hid.c                */
    if (!mgr) return;
    if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
        hid_each_device(mgr, quiet_one, NULL);
        IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    }
    CFRelease(mgr);
}

static bool read_exact(int fd, unsigned char *b, size_t n) {
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

int dsxp_guard_main(int pipe_fd, int sock_fd, int port) {
    signal(SIGINT, SIG_IGN);                  /* only the parent's death matters */
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (port < 1 || port > 65535) port = XP_PORT;
    for (;;) {
        unsigned char ch;
        if (!read_exact(pipe_fd, &ch, 1)) break;   /* EOF or error: parent is gone */
        if (ch == 'Q') return 0;                   /* clean quit */
        if (ch == 'P') {
            unsigned char pb[2];
            if (!read_exact(pipe_fd, pb, 2)) break;
            int p = (pb[0] << 8) | pb[1];
            if (p >= 1) port = p;
        }
    }
    alarm(3);                                      /* never linger, whatever happens */
    release_xplane(sock_fd, port);
    quiet_controller();
    release_xplane(sock_fd, port);
    return 0;
}
