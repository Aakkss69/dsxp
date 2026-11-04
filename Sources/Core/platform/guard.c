/*
 * Crash guard.
 *
 * The controller keeps playing the last output report it received (rumble,
 * trigger resistance), and X-Plane keeps the override datarefs set, if the app
 * dies without cleaning up - a crash, kill -9, or Xcode's Stop button. The app
 * therefore starts a tiny helper: itself, re-executed with --dsxp-guard, holding
 * the read end of a pipe. A clean quit writes 'Q' and the guard leaves quietly.
 * If the pipe closes without 'Q', the parent died: the guard releases X-Plane's
 * controls, quiets the controller, and exits.
 */
#include "core_internal.h"
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

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 3);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* only fd 3 survives into the guard; its own process group, so a Ctrl-C
     * aimed at the app's terminal does not take the guard down with it */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    char *argv[] = { (char *)exe_path, "--dsxp-guard", "3", NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, exe_path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[0]);
    if (rc != 0) { close(fds[1]); return -1; }
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return fds[1];
}

void dsxp_guard_release(int fd) {
    if (fd < 0) return;
    char q = 'Q';
    ssize_t w;
    do { w = write(fd, &q, 1); } while (w < 0 && errno == EINTR);
    close(fd);
}

static void release_xplane(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in xp;
    memset(&xp, 0, sizeof xp);
    xp.sin_family = AF_INET;
    xp.sin_port = htons(XP_PORT);
    inet_pton(AF_INET, XP_IP, &xp.sin_addr);
    static const char *drefs[] = {
        "sim/joystick/yoke_pitch_ratio",
        "sim/joystick/yoke_roll_ratio",
        "sim/joystick/yoke_heading_ratio",
        "sim/operation/override/override_joystick_pitch",
        "sim/operation/override/override_joystick_roll",
        "sim/operation/override/override_joystick_heading",
        "sim/operation/override/override_toe_brakes",
    };
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < sizeof drefs / sizeof drefs[0]; i++) {
            unsigned char p[DREF_LEN];
            size_t n = xp_build_dref(p, drefs[i], 0.0f);
            sendto(s, p, n, 0, (struct sockaddr *)&xp, sizeof xp);
        }
    close(s);
}

static void quiet_controller(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return;
    int vid = DS_VID, pid = DS_PID;
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef vn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey), vn);
    CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), pn);
    IOHIDManagerSetDeviceMatching(mgr, m);
    CFRelease(vn); CFRelease(pn); CFRelease(m);
    if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
        CFSetRef devs = IOHIDManagerCopyDevices(mgr);
        if (devs) {
            CFIndex n = CFSetGetCount(devs);
            const void **arr = malloc(sizeof(void *) * (size_t)(n ? n : 1));
            if (arr) {
                CFSetGetValues(devs, arr);
                for (CFIndex i = 0; i < n; i++) {
                    IOHIDDeviceRef d = (IOHIDDeviceRef)arr[i];
                    CFTypeRef tr = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDTransportKey));
                    char tb[64] = "";
                    if (tr && CFGetTypeID(tr) == CFStringGetTypeID())
                        CFStringGetCString((CFStringRef)tr, tb, sizeof tb, kCFStringEncodingUTF8);
                    if (strcmp(tb, "Bluetooth") != 0) continue;
                    out_frame_t f;
                    memset(&f, 0, sizeof f);
                    unsigned char rep[OUT_REPORT_LEN];
                    out_build_report(&f, 0, rep);
                    IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, BT_REPORT_ID, rep, sizeof rep);
                }
                free(arr);
            }
            CFRelease(devs);
        }
        IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    }
    CFRelease(mgr);
}

int dsxp_guard_main(int fd) {
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1 && ch == 'Q') return 0;         /* clean quit */
        if (n == 1) continue;
        if (n < 0 && errno == EINTR) continue;
        break;                                     /* EOF or error: parent is gone */
    }
    alarm(3);                                      /* never linger, whatever happens */
    release_xplane();
    quiet_controller();
    release_xplane();
    return 0;
}
