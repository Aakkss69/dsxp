/*
 * The running bridge: threads, run loop, HID, UDP, and the public dsxp_* API.
 *
 * Threads
 *   dsxp-core  sole owner of core_t. Its own CFRunLoop hosts the IOHIDManager and
 *              the 60 Hz timer, so neither pauses while the app's menu is open or
 *              a window is being dragged (those run the main loop in a tracking
 *              mode).
 *   dsxp-out   blocking IOHIDDeviceSetReport calls, fed through a one-slot
 *              mailbox (latest report wins), so a congested Bluetooth link never
 *              stalls input decoding.
 *   dsxp.io    serial queue: settings writes and command-catalogue loads.
 * Other threads talk to the core only through the command queue (in) and the
 * published snapshot (out). No lock is ever held across I/O.
 */
#include "core_internal.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAP 256

typedef struct {
    pthread_mutex_t life;
    pthread_cond_t  life_cv;
    bool running, thread_ready, thread_done;
    pthread_t core_th, out_th;
    CFRunLoopRef rl;
    CFRunLoopTimerRef timer;
    atomic_bool stop_req;

    char data_dir[1024], log_dir[1024], settings_path[1100];
    dsxp_event_fn on_event; void *event_ctx;
    dsxp_raw_fn on_raw; void *raw_ctx;
    bool verbose, dry_run;

    /* core thread only */
    core_t core;
    double last_gate;
    IOHIDManagerRef mgr;
    IOHIDDeviceRef bt_dev;
    char status_sig[128];
    atomic_int access;          /* dsxp_access_t, refreshed with the gate */

    int sock;
    struct sockaddr_in xp;

    pthread_mutex_t q_lock;
    cmd_t q[QUEUE_CAP];
    unsigned q_head, q_count;

    pthread_mutex_t snap_lock;
    dsxp_snap_t snap;
    dsxp_status_t status;
    uint64_t seq;

    pthread_rwlock_t cmds_lock;
    xcmd_t *cmds;
    int ncmds;
    char commands_path[1024];

    pthread_mutex_t w_lock;
    pthread_cond_t  w_cv;
    unsigned char w_rep[OUT_REPORT_LEN];
    bool w_pending, w_stop, sync_out;
    IOHIDDeviceRef w_dev;

    dispatch_queue_t io;
    int hid_open_result;
    bool imported;
} runtime_t;

static runtime_t R = {
    .life = PTHREAD_MUTEX_INITIALIZER, .life_cv = PTHREAD_COND_INITIALIZER,
    .q_lock = PTHREAD_MUTEX_INITIALIZER, .snap_lock = PTHREAD_MUTEX_INITIALIZER,
    .cmds_lock = PTHREAD_RWLOCK_INITIALIZER,
    .w_lock = PTHREAD_MUTEX_INITIALIZER, .w_cv = PTHREAD_COND_INITIALIZER,
    .sock = -1,
};

static unsigned char g_bt_buf[512];   /* input buffer of the one Bluetooth device */

double now_sec(void) {
    /* monotonic, keeps counting during sleep, unaffected by clock changes */
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9;
}

/* ---------------------------------------------------------------- sinks */

static void sink_udp(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    if (R.sock < 0 || R.dry_run) return;
    sendto(R.sock, buf, len, 0, (const struct sockaddr *)&R.xp, sizeof R.xp);
}

static void sink_out(void *ctx, const unsigned char *rep) {
    (void)ctx;
    pthread_mutex_lock(&R.w_lock);
    if (R.sync_out) {
        IOHIDDeviceRef d = R.w_dev;
        if (d) CFRetain(d);
        pthread_mutex_unlock(&R.w_lock);
        if (d) {
            IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, BT_REPORT_ID, rep, OUT_REPORT_LEN);
            CFRelease(d);
        }
        return;
    }
    memcpy(R.w_rep, rep, OUT_REPORT_LEN);
    R.w_pending = true;
    pthread_cond_signal(&R.w_cv);
    pthread_mutex_unlock(&R.w_lock);
}

typedef struct { char path[1100]; char *data; size_t len; } save_job_t;

static void save_job_run(void *arg) {
    save_job_t *j = arg;
    if (!file_write_atomic(j->path, j->data, j->len, 0600))
        dsxp_log("cannot write %s", j->path);
    free(j->data);
    free(j);
}

static void sink_save(void *ctx, const char *text, size_t len) {
    (void)ctx;
    save_job_t *j = malloc(sizeof *j);
    if (!j) return;
    j->data = malloc(len ? len : 1);
    if (!j->data) { free(j); return; }
    memcpy(j->data, text, len);
    j->len = len;
    snprintf(j->path, sizeof j->path, "%s", R.settings_path);
    dispatch_async_f(R.io, j, save_job_run);
}

/* --------------------------------------------------------- writer thread */

static void *writer_main(void *arg) {
    (void)arg;
    pthread_setname_np("dsxp-out");
    for (;;) {
        pthread_mutex_lock(&R.w_lock);
        while (!R.w_pending && !R.w_stop) pthread_cond_wait(&R.w_cv, &R.w_lock);
        if (R.w_stop) { pthread_mutex_unlock(&R.w_lock); break; }
        unsigned char rep[OUT_REPORT_LEN];
        memcpy(rep, R.w_rep, sizeof rep);
        R.w_pending = false;
        IOHIDDeviceRef d = R.w_dev;
        if (d) CFRetain(d);
        pthread_mutex_unlock(&R.w_lock);
        if (d) {
            IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, BT_REPORT_ID, rep, sizeof rep);
            CFRelease(d);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ HID */

static bool transport(IOHIDDeviceRef dev, char *out, size_t n) {
    out[0] = 0;
    CFTypeRef tr = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));
    if (!tr || CFGetTypeID(tr) != CFStringGetTypeID()) return false;
    return CFStringGetCString((CFStringRef)tr, out, (CFIndex)n, kCFStringEncodingUTF8);
}

static void report_cb(void *ctx, IOReturn res, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex len) {
    (void)ctx; (void)res; (void)sender;
    if (type != kIOHIDReportTypeInput || reportID != BT_REPORT_ID) return;
    if (R.on_raw) R.on_raw(R.raw_ctx, report, (long)len);
    core_on_report(&R.core, report, (long)len, now_sec());
}

static void set_writer_device(IOHIDDeviceRef dev) {
    pthread_mutex_lock(&R.w_lock);
    if (R.w_dev) CFRelease(R.w_dev);
    R.w_dev = dev ? (IOHIDDeviceRef)CFRetain(dev) : NULL;
    if (!dev) R.w_pending = false;
    pthread_mutex_unlock(&R.w_lock);
}

static void device_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    (void)ctx; (void)res; (void)sender;
    char tb[64];
    transport(dev, tb, sizeof tb);
    if (strcmp(tb, "Bluetooth") != 0) {
        dsxp_log("DualSense on %s - ignoring (USB works natively in X-Plane)", tb[0] ? tb : "?");
        core_on_device(&R.core, false, true);
        return;
    }
    if (R.bt_dev) {
        dsxp_log("a second Bluetooth DualSense was ignored - only one is supported");
        return;
    }
    R.bt_dev = (IOHIDDeviceRef)CFRetain(dev);
    IOHIDDeviceRegisterInputReportCallback(dev, g_bt_buf, sizeof g_bt_buf, report_cb, NULL);
    set_writer_device(dev);
    core_on_device(&R.core, true, true);
    dsxp_log("DualSense on Bluetooth - listening for report 0x%02X", BT_REPORT_ID);
}

static void removal_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    (void)ctx; (void)res; (void)sender;
    if (dev != R.bt_dev) {
        core_on_device(&R.core, false, false);
        dsxp_log("DualSense (non-Bluetooth) removed");
        return;
    }
    set_writer_device(NULL);
    CFRelease(R.bt_dev);
    R.bt_dev = NULL;
    core_on_device(&R.core, true, false);
    dsxp_log("DualSense removed");
}

static void rescan_transports(bool *bt, bool *usb) {
    *bt = *usb = false;
    if (!R.mgr) return;
    CFSetRef devs = IOHIDManagerCopyDevices(R.mgr);
    if (!devs) return;
    CFIndex n = CFSetGetCount(devs);
    const void **arr = malloc(sizeof(void *) * (size_t)(n ? n : 1));
    if (arr) {
        CFSetGetValues(devs, arr);
        for (CFIndex i = 0; i < n; i++) {
            char tb[64];
            if (!transport((IOHIDDeviceRef)arr[i], tb, sizeof tb)) continue;
            if (!strcmp(tb, "Bluetooth")) *bt = true;
            else if (!strcmp(tb, "USB")) *usb = true;
        }
        free(arr);
    }
    CFRelease(devs);
}

static IOHIDManagerRef open_hid(CFRunLoopRef rl, int *result) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) { *result = -1; return NULL; }
    int vid = DS_VID, pid = DS_PID;
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef vn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey),  vn);
    CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), pn);
    IOHIDManagerSetDeviceMatching(mgr, m);
    CFRelease(vn); CFRelease(pn); CFRelease(m);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_cb, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, removal_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, rl, kCFRunLoopDefaultMode);

    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    *result = (int)r;
    if (r != kIOReturnSuccess)
        dsxp_log("IOHIDManagerOpen failed 0x%08x - check Input Monitoring in "
                 "System Settings > Privacy & Security", r);
    return mgr;
}

/* ------------------------------------------------------------- core loop */

static void drain_udp(core_t *c, double now) {
    if (R.sock < 0) return;
    for (int k = 0; k < 64; k++) {
        unsigned char buf[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(R.sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        bool loop = from.sin_family == AF_INET && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
        core_on_udp(c, buf, (size_t)n, loop, now);
    }
}

static void drain_cmds(core_t *c, double now) {
    static cmd_t local[QUEUE_CAP];
    unsigned n = 0;
    pthread_mutex_lock(&R.q_lock);
    while (R.q_count) {
        local[n++] = R.q[R.q_head];
        R.q_head = (R.q_head + 1) % QUEUE_CAP;
        R.q_count--;
    }
    pthread_mutex_unlock(&R.q_lock);
    for (unsigned i = 0; i < n; i++) core_apply_cmd(c, &local[i], now);
}

static dsxp_access_t check_access(void) {
    IOHIDAccessType t = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    return t == kIOHIDAccessTypeGranted ? DSXP_ACCESS_GRANTED :
           t == kIOHIDAccessTypeDenied  ? DSXP_ACCESS_DENIED  : DSXP_ACCESS_UNKNOWN;
}

static void publish(core_t *c, double now) {
    static dsxp_snap_t snap;              /* core thread only; too big for the stack */
    core_snapshot(c, &snap);

    dsxp_status_t st;
    memset(&st, 0, sizeof st);
    st.link = c->bt_present && c->usb_present ? DSXP_LINK_BT_AND_USB :
              c->bt_present ? DSXP_LINK_BT : c->usb_present ? DSXP_LINK_USB : DSXP_LINK_NONE;
    st.input_live = core_input_live(c, now);
    st.xp_running = c->xp_running;
    st.xp_answering = c->wx.live;
    st.engaged = c->engaged;
    st.suspended = c->suspended;
    st.imported_legacy = R.imported;
    st.hid_open_result = R.hid_open_result;
    st.report_hz = c->report_hz;
    snprintf(st.xp_exe_path, sizeof st.xp_exe_path, "%s", c->xp_exe);
    pthread_rwlock_rdlock(&R.cmds_lock);
    st.ncmds = R.ncmds;
    snprintf(st.commands_path, sizeof st.commands_path, "%s", R.commands_path);
    pthread_rwlock_unlock(&R.cmds_lock);

    pthread_mutex_lock(&R.snap_lock);
    R.snap = snap;
    st.seq = ++R.seq;
    R.status = st;
    pthread_mutex_unlock(&R.snap_lock);

    char sig[128];
    snprintf(sig, sizeof sig, "%d%d%d%d%d%d%d%d%d", st.link, st.input_live, st.xp_running,
             st.xp_answering, st.engaged, st.suspended, st.ncmds > 0, st.hid_open_result,
             atomic_load(&R.access));
    if (strcmp(sig, R.status_sig) != 0) {
        snprintf(R.status_sig, sizeof R.status_sig, "%s", sig);
        if (R.on_event) R.on_event(R.event_ctx, DSXP_EV_STATUS);
    }
}

static void noop(void *arg) { (void)arg; }

/* Runs on the core thread, from the timer, once stop was requested. */
static void shutdown_core(void) {
    core_t *c = &R.core;
    xp_disengage(c, now_sec());
    telemetry_unsubscribe(c);

    /* stop the writer, then send the quiet report synchronously */
    pthread_mutex_lock(&R.w_lock);
    R.w_stop = true;
    R.sync_out = true;
    pthread_cond_signal(&R.w_cv);
    pthread_mutex_unlock(&R.w_lock);
    pthread_join(R.out_th, NULL);
    out_quiet(c);

    /* drain queued saves, then write the final state synchronously */
    dispatch_sync_f(R.io, NULL, noop);
    if (c->cfg_dirty) {
        char buf[16384];
        size_t n = bindings_format(&c->s, buf, sizeof buf);
        if (!file_write_atomic(R.settings_path, buf, n, 0600))
            dsxp_log("cannot write %s", R.settings_path);
        c->cfg_dirty = false;
    }
    dsxp_log("stopped. %ld reports processed.", c->reports_total + c->reports_window);
}

static void timer_cb(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    if (atomic_load(&R.stop_req)) {
        shutdown_core();
        CFRunLoopStop(R.rl);
        return;
    }
    double now = now_sec();
    core_t *c = &R.core;
    drain_udp(c, now);
    drain_cmds(c, now);
    if (core_gate_due(c, now, R.last_gate)) {
        R.last_gate = now;
        bool bt, usb;
        rescan_transports(&bt, &usb);
        char exe[1024];
        bool xp = gate_xplane_running(exe, sizeof exe);
        atomic_store(&R.access, (int)check_access());
        core_set_gate(c, bt, usb, xp, exe, now);
    }
    core_tick(c, now);
    publish(c, now);
}

static void *core_main(void *arg) {
    (void)arg;
    pthread_setname_np("dsxp-core");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    R.rl = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());

    R.mgr = open_hid(R.rl, &R.hid_open_result);

    CFRunLoopTimerContext tc = { 0 };
    R.timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.01,
                                   1.0 / TICK_HZ, 0, 0, timer_cb, &tc);
    CFRunLoopTimerSetTolerance(R.timer, 0.001);
    CFRunLoopAddTimer(R.rl, R.timer, kCFRunLoopDefaultMode);

    pthread_mutex_lock(&R.life);
    R.thread_ready = true;
    pthread_cond_broadcast(&R.life_cv);
    pthread_mutex_unlock(&R.life);

    CFRunLoopRun();

    CFRunLoopTimerInvalidate(R.timer);
    CFRelease(R.timer);
    R.timer = NULL;
    if (R.mgr) {
        IOHIDManagerUnscheduleFromRunLoop(R.mgr, R.rl, kCFRunLoopDefaultMode);
        IOHIDManagerClose(R.mgr, kIOHIDOptionsTypeNone);
        CFRelease(R.mgr);
        R.mgr = NULL;
    }
    set_writer_device(NULL);
    if (R.bt_dev) { CFRelease(R.bt_dev); R.bt_dev = NULL; }

    pthread_mutex_lock(&R.life);
    R.thread_done = true;
    pthread_cond_broadcast(&R.life_cv);
    pthread_mutex_unlock(&R.life);
    return NULL;
}

/* ------------------------------------------------------- commands catalogue */

static void load_commands_now(const char *path) {
    size_t len = 0;
    char *text = path && *path ? file_read_all(path, 16u * 1024 * 1024, &len) : NULL;
    xcmd_t *cmds = NULL;
    int n = text ? commands_parse(text, len, &cmds) : 0;
    free(text);

    pthread_rwlock_wrlock(&R.cmds_lock);
    xcmd_t *old = R.cmds;
    R.cmds = cmds;
    R.ncmds = n;
    snprintf(R.commands_path, sizeof R.commands_path, "%s", n > 0 ? path : "");
    pthread_rwlock_unlock(&R.cmds_lock);
    free(old);

    if (n > 0) dsxp_log("loaded %d X-Plane commands from %s", n, path);
    else if (path && *path) dsxp_log("no X-Plane commands found at %s", path);
}

typedef struct { char path[1024]; } cmds_job_t;

static void cmds_job_run(void *arg) {
    cmds_job_t *j = arg;
    load_commands_now(j->path);
    free(j);
    if (R.on_event) R.on_event(R.event_ctx, DSXP_EV_COMMANDS_LOADED);
}

/* ------------------------------------------------------------ public API */

static bool open_udp(void) {
    R.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (R.sock < 0) { dsxp_log("cannot create UDP socket"); return false; }
    fcntl(R.sock, F_SETFD, FD_CLOEXEC);
    fcntl(R.sock, F_SETFL, fcntl(R.sock, F_GETFL) | O_NONBLOCK);
    /*
     * Deliberately NOT binding a fixed source port: 49010 belongs to X-Plane's
     * own RakNet stack, and taking it makes the sim fail with "Failed to
     * initialize UDP RakNet connectivity". An ephemeral port is correct; X-Plane
     * replies to whatever source it sees.
     */
    int rcv = 256 * 1024;   /* room for a burst of RREF replies */
    setsockopt(R.sock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    memset(&R.xp, 0, sizeof R.xp);
    R.xp.sin_family = AF_INET;
    R.xp.sin_port = htons(XP_PORT);
    inet_pton(AF_INET, XP_IP, &R.xp.sin_addr);
    return true;
}

int dsxp_start(const dsxp_config_t *cfg) {
    if (!cfg || !cfg->data_dir || !cfg->log_dir) return -1;
    pthread_mutex_lock(&R.life);
    if (R.running) { pthread_mutex_unlock(&R.life); return -2; }

    atomic_store(&R.stop_req, false);
    R.thread_ready = R.thread_done = false;
    R.q_head = R.q_count = 0;
    R.seq = 0;
    R.status_sig[0] = 0;
    R.bt_dev = NULL;
    R.w_pending = R.w_stop = R.sync_out = false;
    R.w_dev = NULL;
    R.imported = false;
    R.hid_open_result = 0;
    R.last_gate = 0;
    atomic_store(&R.access, (int)DSXP_ACCESS_UNKNOWN);
    memset(&R.status, 0, sizeof R.status);
    snprintf(R.data_dir, sizeof R.data_dir, "%s", cfg->data_dir);
    snprintf(R.log_dir, sizeof R.log_dir, "%s", cfg->log_dir);
    snprintf(R.settings_path, sizeof R.settings_path, "%s/bindings.conf", R.data_dir);
    R.on_event = cfg->on_event; R.event_ctx = cfg->event_ctx;
    R.on_raw = cfg->on_raw_report; R.raw_ctx = cfg->raw_ctx;
    R.verbose = cfg->verbose;
    R.dry_run = cfg->dry_run;

    mkdir_p(R.data_dir, 0700);
    mkdir_p(R.log_dir, 0755);
    log_init(R.log_dir, cfg->log_retention_days, cfg->verbose);
    if (!R.io) {
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        R.io = dispatch_queue_create("dsxp.io", attr);
    }

    /* settings: our own file, else a one-time import of the prototype's, else defaults */
    settings_t s;
    load_defaults(&s);
    bool need_save = false;
    size_t len = 0;
    char *text = file_read_all(R.settings_path, SETTINGS_MAX_BYTES, &len);
    if (text) {
        bindings_parse(&s, text, len);
        dsxp_log("loaded settings from %s", R.settings_path);
    } else if (cfg->import_bindings && *cfg->import_bindings &&
               (text = file_read_all(cfg->import_bindings, SETTINGS_MAX_BYTES, &len)) != NULL) {
        bindings_parse(&s, text, len);
        R.imported = true;
        need_save = true;
        dsxp_log("imported settings from %s", cfg->import_bindings);
    } else {
        need_save = true;
        dsxp_log("no settings yet - using profile defaults");
    }
    free(text);

    open_udp();
    load_commands_now(cfg->commands_path);

    core_sinks_t sinks = { NULL, sink_udp, sink_out, sink_save };
    core_init(&R.core, &s, &sinks);
    R.core.verbose = R.verbose;
    R.core.dry_run = R.dry_run;
    R.core.imported = R.imported;
    if (need_save) { R.core.cfg_dirty = true; R.core.cfg_dirty_at = 0; }

    /* Backstop for a previous instance that died without cleanup: if X-Plane is
     * up, make sure no stale override is holding its controls. */
    if (!R.dry_run && R.sock >= 0) {
        char exe[1024];
        if (gate_xplane_running(exe, sizeof exe)) {
            static const char *ov[] = {
                "sim/operation/override/override_joystick_pitch",
                "sim/operation/override/override_joystick_roll",
                "sim/operation/override/override_joystick_heading",
                "sim/operation/override/override_toe_brakes" };
            for (int i = 0; i < 4; i++) {
                unsigned char p[DREF_LEN];
                sink_udp(NULL, p, xp_build_dref(p, ov[i], 0.0f));
            }
        }
    }

    if (pthread_create(&R.out_th, NULL, writer_main, NULL) != 0) {
        dsxp_log("cannot start the output thread");
        pthread_mutex_unlock(&R.life);
        return -3;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int rc = pthread_create(&R.core_th, &attr, core_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        dsxp_log("cannot start the core thread");
        pthread_mutex_lock(&R.w_lock);
        R.w_stop = true;
        pthread_cond_signal(&R.w_cv);
        pthread_mutex_unlock(&R.w_lock);
        pthread_mutex_unlock(&R.life);
        pthread_join(R.out_th, NULL);
        return -4;
    }
    while (!R.thread_ready) pthread_cond_wait(&R.life_cv, &R.life);
    R.running = true;
    pthread_mutex_unlock(&R.life);
    return 0;
}

void dsxp_stop(double timeout_sec) {
    pthread_mutex_lock(&R.life);
    if (!R.running) { pthread_mutex_unlock(&R.life); return; }
    atomic_store(&R.stop_req, true);
    if (R.rl) CFRunLoopWakeUp(R.rl);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double secs = timeout_sec > 0 ? timeout_sec : 1.0;
    ts.tv_sec += (time_t)secs;
    ts.tv_nsec += (long)((secs - (double)(time_t)secs) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (!R.thread_done)
        if (pthread_cond_timedwait(&R.life_cv, &R.life, &ts) == ETIMEDOUT) break;
    bool done = R.thread_done;
    R.running = false;
    pthread_mutex_unlock(&R.life);

    if (done) {
        pthread_join(R.core_th, NULL);
        if (R.rl) { CFRelease(R.rl); R.rl = NULL; }
        if (R.sock >= 0) { close(R.sock); R.sock = -1; }
    } else {
        dsxp_log("core thread did not stop within %.1f s", secs);
    }
    log_shutdown();
}

bool dsxp_is_running(void) {
    pthread_mutex_lock(&R.life);
    bool r = R.running;
    pthread_mutex_unlock(&R.life);
    return r;
}

void dsxp_get_status(dsxp_status_t *out) {
    pthread_mutex_lock(&R.snap_lock);
    *out = R.status;
    pthread_mutex_unlock(&R.snap_lock);
}

void dsxp_set_commands_path(const char *path) {
    cmds_job_t *j = malloc(sizeof *j);
    if (!j) return;
    snprintf(j->path, sizeof j->path, "%s", path ? path : "");
    if (R.io) dispatch_async_f(R.io, j, cmds_job_run);
    else cmds_job_run(j);
}

static bool runtime_submit(void *ctx, const cmd_t *cmd) {
    (void)ctx;
    pthread_mutex_lock(&R.q_lock);
    bool okq = R.q_count < QUEUE_CAP;
    if (okq) {
        R.q[(R.q_head + R.q_count) % QUEUE_CAP] = *cmd;
        R.q_count++;
    }
    pthread_mutex_unlock(&R.q_lock);
    return okq;
}

void dsxp_set_suspended(bool suspended) {
    cmd_t c = { .kind = CMD_SUSPEND };
    c.u.flag = suspended;
    runtime_submit(NULL, &c);
}

dsxp_access_t dsxp_input_access(bool request) {
    dsxp_access_t a = check_access();
    if (request && a != DSXP_ACCESS_GRANTED) {
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        a = check_access();
    }
    atomic_store(&R.access, (int)a);
    return a;
}

static void runtime_snapshot(void *ctx, dsxp_snap_t *out) {
    (void)ctx;
    pthread_mutex_lock(&R.snap_lock);
    *out = R.snap;
    pthread_mutex_unlock(&R.snap_lock);
}

static void runtime_catalogue(void *ctx, void (*fn)(void *, const xcmd_t *, int), void *arg) {
    (void)ctx;
    pthread_rwlock_rdlock(&R.cmds_lock);
    fn(arg, R.cmds, R.ncmds);
    pthread_rwlock_unlock(&R.cmds_lock);
}

static void runtime_app_info(void *ctx, jbuf_t *j) {
    (void)ctx;
    dsxp_status_t st;
    dsxp_get_status(&st);
    int a = atomic_load(&R.access);
    const char *access = a == DSXP_ACCESS_GRANTED ? "granted" :
                         a == DSXP_ACCESS_DENIED ? "denied" : "unknown";
    /* the X-Plane folder is the catalogue path minus Resources/plugins/Commands.txt */
    char xpdir[1024];
    snprintf(xpdir, sizeof xpdir, "%s", st.commands_path);
    const char *tail = "/Resources/plugins/Commands.txt";
    size_t n = strlen(xpdir), tn = strlen(tail);
    if (n > tn && !strcmp(xpdir + n - tn, tail)) xpdir[n - tn] = 0;
    else xpdir[0] = 0;

    jb_printf(j, "{\"access\":\"%s\",\"cmds\":%d,\"xpdir\":", access, st.ncmds);
    jb_str(j, xpdir);
    jb_printf(j, ",\"imported\":%s,\"xp_answering\":%s,\"input_live\":%s,\"hid_open\":%d,"
                 "\"hz\":%.1f,\"suspended\":%s}",
              st.imported_legacy ? "true" : "false", st.xp_answering ? "true" : "false",
              st.input_live ? "true" : "false", st.hid_open_result, st.report_hz,
              st.suspended ? "true" : "false");
}

void dsxp_api(const char *method, const char *path, const char *query, dsxp_response_t *out) {
    api_env_t env = { NULL, runtime_snapshot, runtime_submit, runtime_catalogue, runtime_app_info };
    api_handle(&env, method, path, query, out);
}

void dsxp_response_free(dsxp_response_t *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
