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
 *   dsxp.io    serial queue: settings writes and command-catalog loads.
 *   dsxp.cal   serial queue: reads the controller's factory calibration (feature
 *              report 0x05) once per connection. Its own queue, because a Bluetooth
 *              read can block for seconds and must delay neither saves nor quit.
 *              The read and dsxp-out's writes take turns (w_busy / cal_reading under
 *              w_lock): IOKit documents no thread safety for two transfers to one
 *              device at once, so they are never in flight together.
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
#include <math.h>
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

    char data_dir[1024], log_dir[1024];

    /* config files (core thread), and what is published about them (snap_lock) */
    cfg_store_t store;
    double last_cfg_poll;
    double timer_hz;            /* rate the live CFRunLoopTimer was created with */
    conf_warns_t pub_warns[CF_COUNT];
    unsigned pub_gen;
    int pub_last_reload;
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
    bool w_pending, w_stop, w_done, sync_out;
    bool w_busy;                /* dsxp-out is inside IOHIDDeviceSetReport              */
    bool cal_reading;           /* dsxp.cal is inside IOHIDDeviceGetReport: writes wait  */
    IOHIDDeviceRef w_dev;

    dispatch_queue_t io;
    int hid_open_result;
    bool imported;
    bool zombie;                /* a stop timed out: threads may still be alive */
    atomic_int want_suspend;    /* -1 none, 0 resume, 1 suspend (F21) */
    atomic_uint cmds_gen;       /* bumped on every catalog load (F17) */
    atomic_uint save_failed;    /* config files whose background save failed (1 << f),
                                   set on dsxp.io, taken by the core thread             */
    unsigned save_retry;        /* core thread: files to save again ...                  */
    double save_retry_at;       /*   ... once this time has come (SAVE_RETRY_SEC apart)  */

    /* factory calibration (report 0x05): read on dsxp.cal, handed to the core thread */
    dispatch_queue_t cal_q;
    atomic_uint dev_gen;        /* +1 per Bluetooth connect and disconnect; never reset,
                                   so a read still running from an earlier connection (or
                                   an earlier start) can never be taken for the current one */
    pthread_mutex_t cal_lock;   /* guards cal_slot                                         */
    struct { unsigned gen; motion_cal_t cal; mcal_result_t res; int ior; } cal_slot;
    atomic_bool cal_ready;      /* cal_slot holds a result the core has not taken yet      */
} runtime_t;

static runtime_t R = {
    .life = PTHREAD_MUTEX_INITIALIZER, .life_cv = PTHREAD_COND_INITIALIZER,
    .q_lock = PTHREAD_MUTEX_INITIALIZER, .snap_lock = PTHREAD_MUTEX_INITIALIZER,
    .cmds_lock = PTHREAD_RWLOCK_INITIALIZER,
    .w_lock = PTHREAD_MUTEX_INITIALIZER, .w_cv = PTHREAD_COND_INITIALIZER,
    .cal_lock = PTHREAD_MUTEX_INITIALIZER,
    .sock = -1,
    .want_suspend = -1,
};

bool runtime_test_disable_hid = false;   /* tests: run the core without IOKit HID */

int runtime_udp_socket(void) { return R.sock; }
int runtime_xp_port(void) { return R.sock >= 0 ? ntohs(R.xp.sin_port) : XP_PORT; }

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

/* A background save that failed is tried again this long after (not at once: a full or
 * read-only disk would otherwise fill the log). */
#define SAVE_RETRY_SEC 10.0

typedef struct { char path[1100]; char *data; size_t len; int file; } save_job_t;
static void save_job_noop(void *arg) { (void)arg; }

static void save_job_run(void *arg) {
    save_job_t *j = arg;
    if (!file_write_atomic(j->path, j->data, j->len, 0644)) {
        dsxp_log("cannot write %s", j->path);
        /* the store already counts this text as saved; tell the core thread, which
         * forgets that and saves the file again (config_tick) */
        if (j->file >= 0) atomic_fetch_or(&R.save_failed, 1u << j->file);
    }
    free(j->data);
    free(j);
}

/* The config store writes through here: synchronously at start and quit, on the
 * io queue otherwise so the core loop never waits for the disk. */
static bool store_write(void *ctx, const char *path, const char *text, size_t len, bool sync) {
    (void)ctx;
    if (sync || !R.io) {
        if (R.io) dispatch_sync_f(R.io, NULL, save_job_noop);      /* keep writes in order */
        bool okw = file_write_atomic(path, text, len, 0644);
        if (!okw) dsxp_log("cannot write %s", path);
        return okw;
    }
    save_job_t *j = malloc(sizeof *j);
    if (!j) return false;
    j->data = malloc(len ? len : 1);
    if (!j->data) { free(j); return false; }
    memcpy(j->data, text, len);
    j->len = len;
    snprintf(j->path, sizeof j->path, "%s", path);
    j->file = -1;                                  /* which config file, to retry it    */
    for (int f = 0; f < CF_COUNT; f++)
        if (!strcmp(path, R.store.path[f])) j->file = f;
    dispatch_async_f(R.io, j, save_job_run);
    return true;
}

/* --------------------------------------------------------- writer thread */

static void *writer_main(void *arg) {
    (void)arg;
    pthread_setname_np("dsxp-out");
    for (;;) {
        pthread_mutex_lock(&R.w_lock);
        /* wait for a report, and while the calibration read runs (at quit the last
         * report, the quiet one, goes out regardless: stopping the motors matters more) */
        while ((!R.w_pending || R.cal_reading) && !R.w_stop) pthread_cond_wait(&R.w_cv, &R.w_lock);
        if (!R.w_pending) {                 /* stop requested, nothing left to send */
            R.w_done = true;
            pthread_cond_broadcast(&R.w_cv);
            pthread_mutex_unlock(&R.w_lock);
            break;
        }
        unsigned char rep[OUT_REPORT_LEN];
        memcpy(rep, R.w_rep, sizeof rep);
        R.w_pending = false;
        IOHIDDeviceRef d = R.w_dev;
        if (d) CFRetain(d);
        R.w_busy = d != NULL;                   /* a calibration read waits for this one */
        pthread_mutex_unlock(&R.w_lock);
        if (d) {
            IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, BT_REPORT_ID, rep, sizeof rep);
            CFRelease(d);
            pthread_mutex_lock(&R.w_lock);
            R.w_busy = false;
            pthread_cond_broadcast(&R.w_cv);    /* wake a waiting calibration read       */
            pthread_mutex_unlock(&R.w_lock);
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

/* ------------------------------------------------ factory calibration (0x05) */

/* Hand a calibration result to the core thread (any thread). The latest result wins;
 * its generation says which connection it belongs to. */
void runtime_cal_post(unsigned gen, const motion_cal_t *cal, mcal_result_t res, int ior) {
    pthread_mutex_lock(&R.cal_lock);
    R.cal_slot.gen = gen;
    R.cal_slot.cal = *cal;
    R.cal_slot.res = res;
    R.cal_slot.ior = ior;
    pthread_mutex_unlock(&R.cal_lock);
    atomic_store(&R.cal_ready, true);            /* after the slot is complete           */
}

/* Core thread, every tick: apply a posted result if it is for the controller connected
 * now; a result for one that has since gone (or reconnected) is dropped. Returns true
 * when a result was applied. */
bool runtime_cal_drain(core_t *c) {
    if (!atomic_exchange(&R.cal_ready, false)) return false;   /* nothing new            */
    pthread_mutex_lock(&R.cal_lock);
    unsigned gen = R.cal_slot.gen;
    motion_cal_t cal = R.cal_slot.cal;
    mcal_result_t res = R.cal_slot.res;
    int ior = R.cal_slot.ior;
    pthread_mutex_unlock(&R.cal_lock);
    if (gen != atomic_load(&R.dev_gen)) {
        dsxp_log("factory calibration: result for an earlier connection dropped");
        return false;
    }
    if (ior != kIOReturnSuccess)
        dsxp_log("factory calibration: report 0x%02X not read (IOReturn 0x%08x)",
                 MOTION_CAL_REPORT_ID, (unsigned)ior);
    core_set_motion_cal(c, &cal, res);           /* logs the outcome                       */
    return true;
}

unsigned runtime_dev_gen(void) { return atomic_load(&R.dev_gen); }

typedef struct { IOHIDDeviceRef dev; unsigned gen; } cal_job_t;

/* dsxp.cal: read feature report 0x05, up to three tries (Bluetooth can be busy just
 * after a connect), then post the result. Gives up early if the controller went away. */
static void cal_job_run(void *arg) {
    cal_job_t *j = arg;
    motion_cal_t cal;
    motion_cal_nominal(&cal);                    /* what an unread controller gets         */
    mcal_result_t res = MCAL_SHORT;
    IOReturn ior = kIOReturnError;
    for (int attempt = 0; attempt < 3 && atomic_load(&R.dev_gen) == j->gen; attempt++) {
        if (attempt) usleep(200 * 1000);         /* a short pause before trying again      */
        unsigned char buf[64] = { MOTION_CAL_REPORT_ID };   /* report ID in byte 0       */
        CFIndex len = sizeof buf;
        pthread_mutex_lock(&R.w_lock);           /* take turns with dsxp-out's writes:     */
        R.cal_reading = true;                    /*   no new write starts from here on,   */
        while (R.w_busy) pthread_cond_wait(&R.w_cv, &R.w_lock);   /* one in flight ends  */
        pthread_mutex_unlock(&R.w_lock);
        ior = IOHIDDeviceGetReport(j->dev, kIOHIDReportTypeFeature, MOTION_CAL_REPORT_ID,
                                   buf, &len);   /* blocking: why this runs off the core   */
        pthread_mutex_lock(&R.w_lock);
        R.cal_reading = false;                   /* writes may go again: the latest report */
        pthread_cond_broadcast(&R.w_cv);         /*   waiting in the mailbox goes out now  */
        pthread_mutex_unlock(&R.w_lock);
        if (ior != kIOReturnSuccess) continue;
        res = motion_cal_parse(buf, (long)len, &cal);   /* nominal scales on failure       */
        if (res == MCAL_OK) break;
    }
    runtime_cal_post(j->gen, &cal, res, (int)ior);
    CFRelease(j->dev);                           /* retained by request_calibration        */
    free(j);
}

/* Core thread, on a Bluetooth connect: start reading the calibration. */
static void request_calibration(IOHIDDeviceRef dev) {
    unsigned gen = atomic_fetch_add(&R.dev_gen, 1) + 1;   /* this connection's number       */
    if (!R.cal_q) return;                        /* not started through dsxp_start         */
    cal_job_t *j = malloc(sizeof *j);
    if (!j) return;                              /* nominal scales stay in use             */
    j->dev = (IOHIDDeviceRef)CFRetain(dev);      /* kept alive while the read runs         */
    j->gen = gen;
    dispatch_async_f(R.cal_q, j, cal_job_run);
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
    request_calibration(dev);
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
    atomic_fetch_add(&R.dev_gen, 1);             /* a calibration read still running is stale */
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
    core_snapshot(c, now, &snap);

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
    st.xp_port = c->bridge.port;
    snprintf(st.xp_exe_path, sizeof st.xp_exe_path, "%s", c->xp_exe);
    /* the catalog changes rarely: take its lock only when the generation moved,
     * not 60 times a second (F17) */
    static unsigned seen_gen = (unsigned)-1;
    static int cached_ncmds;
    static char cached_path[1024];
    unsigned gen = atomic_load(&R.cmds_gen);
    if (gen != seen_gen) {
        pthread_rwlock_rdlock(&R.cmds_lock);
        cached_ncmds = R.ncmds;
        snprintf(cached_path, sizeof cached_path, "%s", R.commands_path);
        pthread_rwlock_unlock(&R.cmds_lock);
        seen_gen = gen;
    }
    st.ncmds = cached_ncmds;
    snprintf(st.commands_path, sizeof st.commands_path, "%s", cached_path);

    pthread_mutex_lock(&R.snap_lock);
    R.snap = snap;
    st.seq = ++R.seq;
    for (int f = 0; f < CF_COUNT; f++) {
        st.config_warnings += R.pub_warns[f].n;
        if (R.pub_warns[f].n && !st.config_warn_file[0])
            snprintf(st.config_warn_file, sizeof st.config_warn_file, "%s", conf_schema((conf_file_t)f)->file);
    }
    R.status = st;
    pthread_mutex_unlock(&R.snap_lock);

    char sig[160];
    snprintf(sig, sizeof sig, "%d%d%d%d%d%d%d%d%d|%d|%d", st.link, st.input_live, st.xp_running,
             st.xp_answering, st.engaged, st.suspended, st.ncmds > 0, st.hid_open_result,
             atomic_load(&R.access), st.config_warnings, st.xp_port);
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

    /* The quiet report goes through the writer like any other; then the writer is
     * asked to finish. A Bluetooth write can block (weak link, controller out of
     * range), so it gets 0.3 s and is then left behind instead of holding up quit;
     * the crash guard covers the controller in that case (F1). */
    out_quiet(c);
    pthread_mutex_lock(&R.w_lock);
    R.w_stop = true;
    pthread_cond_broadcast(&R.w_cv);
    double deadline = now_sec() + 0.3;
    while (!R.w_done) {
        double left = deadline - now_sec();
        if (left <= 0) break;
        struct timespec rel = { (time_t)left, (long)((left - (double)(time_t)left) * 1e9) };
        pthread_cond_timedwait_relative_np(&R.w_cv, &R.w_lock, &rel);
    }
    bool writer_done = R.w_done;
    pthread_mutex_unlock(&R.w_lock);
    if (writer_done) pthread_join(R.out_th, NULL);
    else {
        pthread_detach(R.out_th);
        R.zombie = true;
        dsxp_log("a controller write was still blocked at quit; left it behind");
    }

    /* drain queued saves, then write what the mapper still had pending */
    dispatch_sync_f(R.io, NULL, noop);
    if (c->cfg_dirty_mask) {
        static config_t live;
        core_get_config(c, &live);
        cfg_store_flush(&R.store, &live, c->cfg_dirty_mask, wall_now(), true, NULL);
        c->cfg_dirty_mask = 0;
    }
    dsxp_log("stopped. %ld reports processed.", c->reports_total + c->reports_window);
}

static void timer_cb(CFRunLoopTimerRef t, void *info);

/* timing.tick_rate changed: replace the timer. Legal from the timer's own
 * callback; the run loop keeps the old one alive until the callback returns. */
static void retime_timer(double hz) {
    if (!(hz >= 1)) return;
    CFRunLoopTimerContext tc = { 0 };
    CFRunLoopTimerRef nt = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1.0 / hz,
                                                1.0 / hz, 0, 0, timer_cb, &tc);
    CFRunLoopTimerSetTolerance(nt, 0.1 / hz);
    CFRunLoopAddTimer(R.rl, nt, kCFRunLoopDefaultMode);
    if (R.timer) { CFRunLoopTimerInvalidate(R.timer); CFRelease(R.timer); }
    R.timer = nt;
    R.timer_hz = hz;
    dsxp_log("core loop now runs at %.0f Hz", hz);
}

static void publish_config(void) {
    pthread_mutex_lock(&R.snap_lock);
    memcpy(R.pub_warns, R.store.warns, sizeof R.pub_warns);
    R.pub_gen = R.store.gen;
    R.pub_last_reload = R.store.last_reload;
    pthread_mutex_unlock(&R.snap_lock);
}

/* bridge.conf changed while running: apply what needs more than a new value. */
static void apply_bridge_runtime(core_t *c, const bridge_cfg_t *old, double now) {
    if (c->bridge.port != old->port) {
        /* hand X-Plane back on the old port before talking to the new one: every
         * release repeat goes out now, while the address is still the old one */
        xp_disengage(c, now);
        xp_release_now(c);
        telemetry_unsubscribe(c);
        sim_release(c, "X-Plane port changed");
        R.xp.sin_port = htons((uint16_t)c->bridge.port);
        c->force_gate = true;
        dsxp_log("X-Plane port is now %d", c->bridge.port);
    }
    if (c->bridge.telemetry_hz != old->telemetry_hz && c->subscribed) {
        c->subscribed = false;              /* the next tick re-subscribes at the new rate */
        c->last_sub = 0;
    }
    if (c->bridge.log_retention_days != old->log_retention_days ||
        c->bridge.log_file_cap_mb != old->log_file_cap_mb || c->bridge.log_verbose != old->log_verbose)
        log_configure(c->bridge.log_retention_days, (long)c->bridge.log_file_cap_mb * 1024 * 1024,
                      R.verbose || c->bridge.log_verbose);
    c->verbose = R.verbose || c->bridge.log_verbose;
}

/* Every CONFIG_POLL_SEC: apply edits made outside DSXP. Every tick: save what
 * the mapper changed once save_delay has passed. */
static void config_tick(core_t *c, double now) {
    static config_t live;
    /* a background save that failed: forget its hash (or the retry would be skipped as
     * "nothing changed") and save that file again, SAVE_RETRY_SEC apart */
    unsigned failed = atomic_exchange(&R.save_failed, 0);
    for (int f = 0; f < CF_COUNT; f++)
        if (failed & (1u << f)) R.store.hash[f] = 0;
    R.save_retry |= failed;
    if (R.save_retry && now >= R.save_retry_at) {
        core_mark_dirty(c, now, R.save_retry);
        R.save_retry = 0;
        R.save_retry_at = now + SAVE_RETRY_SEC;
    }
    if (now - R.last_cfg_poll >= CONFIG_POLL_SEC) {
        R.last_cfg_poll = now;
        core_get_config(c, &live);
        bridge_cfg_t old = c->bridge;
        unsigned gen = R.store.gen;
        unsigned ch = cfg_store_poll(&R.store, &live, c->cfg_dirty_mask, wall_now());
        if (ch) {
            core_apply_config(c, &live, ch, now);
            if (ch & CFB_BRIDGE) apply_bridge_runtime(c, &old, now);
        }
        if (ch || R.store.gen != gen) publish_config();   /* warnings too (unreadable file) */
    }
    unsigned due = core_save_due(c, now);
    if (due) {
        core_get_config(c, &live);
        bridge_cfg_t old = c->bridge;
        unsigned later = 0;
        unsigned ch = cfg_store_flush(&R.store, &live, due, wall_now(), false, &later);
        if (later) core_mark_dirty(c, now, later);   /* an edit still landing: save after it */
        if (ch) {                            /* an outside edit was merged in */
            core_apply_config(c, &live, ch, now);
            if (ch & CFB_BRIDGE) apply_bridge_runtime(c, &old, now);
            publish_config();
        }
    }
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
    runtime_cal_drain(c);
    int ws = atomic_exchange(&R.want_suspend, -1);
    if (ws >= 0) core_set_suspended(c, ws == 1, now);
    if (core_gate_due(c, now, R.last_gate)) {
        R.last_gate = now;
        bool bt, usb;
        rescan_transports(&bt, &usb);
        char exe[1024];
        bool xp = gate_xplane_running(exe, sizeof exe);
        atomic_store(&R.access, (int)check_access());
        /* Input Monitoring granted after launch: open the HID manager now instead of
         * waiting for a relaunch (F22) */
        if (R.hid_open_result != 0 && R.mgr && atomic_load(&R.access) == DSXP_ACCESS_GRANTED) {
            IOReturn r = IOHIDManagerOpen(R.mgr, kIOHIDOptionsTypeNone);
            R.hid_open_result = (int)r;
            if (r == kIOReturnSuccess) dsxp_log("input access granted - controller access opened");
        }
        core_set_gate(c, bt, usb, xp, exe, now);
    }
    core_tick(c, now);
    config_tick(c, now);
    publish(c, now);
    if (R.timer_hz != c->bridge.tick_hz) retime_timer(c->bridge.tick_hz);
}

static void *core_main(void *arg) {
    (void)arg;
    pthread_setname_np("dsxp-core");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    R.rl = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());

    R.mgr = runtime_test_disable_hid ? NULL : open_hid(R.rl, &R.hid_open_result);

    CFRunLoopTimerContext tc = { 0 };
    R.timer_hz = R.core.bridge.tick_hz;
    R.timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.01,
                                   1.0 / R.timer_hz, 0, 0, timer_cb, &tc);
    CFRunLoopTimerSetTolerance(R.timer, 0.1 / R.timer_hz);
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

/* ------------------------------------------------------- commands catalog */

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
    atomic_fetch_add(&R.cmds_gen, 1);
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

static bool open_udp(int port) {
    R.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (R.sock < 0) { dsxp_log("cannot create UDP socket"); return false; }
    fcntl(R.sock, F_SETFD, FD_CLOEXEC);
    fcntl(R.sock, F_SETFL, fcntl(R.sock, F_GETFL) | O_NONBLOCK);
    /*
     * Bound to 127.0.0.1 with an ephemeral port: only this Mac can send to it,
     * and no fixed port is taken - 49010 belongs to X-Plane's own RakNet stack,
     * and taking it makes the sim fail with "Failed to initialize UDP RakNet
     * connectivity". X-Plane replies to whatever source it sees. The system's
     * default receive buffer (net.inet.udp.recvspace) is already larger than
     * the 256 KB the prototype asked for, so it is left alone (F16).
     */
    struct sockaddr_in me;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = 0;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(R.sock, (const struct sockaddr *)&me, sizeof me) != 0)
        dsxp_log("cannot bind the UDP socket to loopback");
    memset(&R.xp, 0, sizeof R.xp);
    R.xp.sin_family = AF_INET;
    R.xp.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, XP_IP, &R.xp.sin_addr);
    return true;
}

/* Undo what a failed start opened (F20). */
static void start_rollback(void) {
    if (R.sock >= 0) { close(R.sock); R.sock = -1; }
    log_shutdown();
}

int dsxp_start(const dsxp_config_t *cfg) {
    if (!cfg || !cfg->data_dir || !cfg->log_dir) return -1;
    pthread_mutex_lock(&R.life);
    if (R.running) { pthread_mutex_unlock(&R.life); return -2; }
    if (R.zombie) { pthread_mutex_unlock(&R.life); return -5; }   /* earlier stop timed out */
    /* paths that would be cut short would point at the wrong files (F19) */
    if (strlen(cfg->data_dir) + sizeof "/bindings.conf.tmp.4294967295" >= sizeof R.data_dir ||
        strlen(cfg->log_dir) + 32 >= sizeof R.log_dir) {
        pthread_mutex_unlock(&R.life);
        return -1;
    }

    atomic_store(&R.stop_req, false);
    R.thread_ready = R.thread_done = false;
    R.q_head = R.q_count = 0;
    R.seq = 0;
    R.status_sig[0] = 0;
    R.bt_dev = NULL;
    R.w_pending = R.w_stop = R.w_done = R.sync_out = false;
    R.w_busy = false;               /* cal_reading is not reset: a read from an earlier
                                       start may still run, and clears it itself         */
    atomic_store(&R.want_suspend, -1);
    R.w_dev = NULL;
    R.imported = false;
    R.hid_open_result = 0;
    R.last_gate = 0;
    atomic_store(&R.access, (int)DSXP_ACCESS_UNKNOWN);
    memset(&R.status, 0, sizeof R.status);
    R.save_retry = 0;                       /* nothing left over from an earlier run   */
    R.save_retry_at = 0;
    atomic_store(&R.save_failed, 0);
    snprintf(R.data_dir, sizeof R.data_dir, "%s", cfg->data_dir);
    snprintf(R.log_dir, sizeof R.log_dir, "%s", cfg->log_dir);
    R.on_event = cfg->on_event; R.event_ctx = cfg->event_ctx;
    R.on_raw = cfg->on_raw_report; R.raw_ctx = cfg->raw_ctx;
    R.verbose = cfg->verbose;
    R.dry_run = cfg->dry_run;

    mkdir_p(R.data_dir, 0700);
    mkdir_p(R.log_dir, 0755);
    log_init(R.log_dir, BRIDGE_DEFAULTS.log_retention_days, cfg->verbose);
    if (!R.io) {
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        R.io = dispatch_queue_create("dsxp.io", attr);
    }
    if (!R.cal_q) {                              /* kept across restarts, like dsxp.io      */
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        R.cal_q = dispatch_queue_create("dsxp.cal", attr);
    }

    /* the three config files: written on first run, older settings imported once */
    static config_t conf;
    if (cfg_store_open(&R.store, R.data_dir, cfg->import_bindings, &conf, wall_now(), store_write, NULL) != 0) {
        start_rollback();
        pthread_mutex_unlock(&R.life);
        return -1;
    }
    R.imported = R.store.imported_prototype || R.store.migrated_v2;
    log_configure(conf.bridge.log_retention_days, (long)conf.bridge.log_file_cap_mb * 1024 * 1024,
                  cfg->verbose || conf.bridge.log_verbose);
    publish_config();
    R.last_cfg_poll = now_sec();

    open_udp(conf.bridge.port);
    load_commands_now(cfg->commands_path);

    core_sinks_t sinks = { NULL, sink_udp, sink_out };
    core_init(&R.core, &conf.s, &sinks);
    R.core.bridge = conf.bridge;
    R.core.verbose = R.verbose || conf.bridge.log_verbose;
    R.core.dry_run = R.dry_run;
    R.core.imported = R.imported;

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
        start_rollback();
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
        pthread_join(R.out_th, NULL);
        start_rollback();
        pthread_mutex_unlock(&R.life);
        return -4;
    }
    while (!R.thread_ready) pthread_cond_wait(&R.life_cv, &R.life);
    R.running = true;
    pthread_mutex_unlock(&R.life);
    return 0;
}

bool dsxp_stop(double timeout_sec) {
    pthread_mutex_lock(&R.life);
    if (!R.running) { pthread_mutex_unlock(&R.life); return !R.zombie; }
    atomic_store(&R.stop_req, true);
    if (R.rl) CFRunLoopWakeUp(R.rl);

    /* Relative waits on the monotonic clock: a wall-clock step during quit can
     * neither cut the wait short nor stretch it (F13). */
    double secs = isfinite(timeout_sec) ? timeout_sec : 1.0;
    if (secs < 0.1) secs = 0.1;
    if (secs > 10.0) secs = 10.0;
    double deadline = now_sec() + secs;
    while (!R.thread_done) {
        double left = deadline - now_sec();
        if (left <= 0) break;
        struct timespec rel = { (time_t)left, (long)((left - (double)(time_t)left) * 1e9) };
        pthread_cond_timedwait_relative_np(&R.life_cv, &R.life, &rel);
    }
    bool done = R.thread_done;
    R.running = false;
    if (!done) R.zombie = true;
    pthread_mutex_unlock(&R.life);

    if (done) {
        pthread_join(R.core_th, NULL);
        if (R.rl) { CFRelease(R.rl); R.rl = NULL; }
        if (R.sock >= 0) { close(R.sock); R.sock = -1; }
    } else {
        dsxp_log("core thread did not stop within %.1f s", secs);
    }
    log_shutdown();
    return done && !R.zombie;
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
    /* an atomic, not the command ring: a full ring must never lose a resume (F21) */
    atomic_store(&R.want_suspend, suspended ? 1 : 0);
    if (R.rl) CFRunLoopWakeUp(R.rl);
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

static void runtime_catalog(void *ctx, void (*fn)(void *, const xcmd_t *, int), void *arg) {
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
    /* the X-Plane folder is the catalog path minus Resources/plugins/Commands.txt */
    char xpdir[1024];
    snprintf(xpdir, sizeof xpdir, "%s", st.commands_path);
    const char *tail = "/Resources/plugins/Commands.txt";
    size_t n = strlen(xpdir), tn = strlen(tail);
    if (n > tn && !strcmp(xpdir + n - tn, tail)) xpdir[n - tn] = 0;
    else xpdir[0] = 0;

    jb_printf(j, "{\"access\":\"%s\",\"cmds\":%d,\"xpdir\":", access, st.ncmds);
    jb_str(j, xpdir);
    jb_printf(j, ",\"imported\":%s,\"xp_answering\":%s,\"input_live\":%s,\"hid_open\":%d,"
                 "\"hz\":%.1f,\"suspended\":%s,\"config\":{\"dir\":",
              st.imported_legacy ? "true" : "false", st.xp_answering ? "true" : "false",
              st.input_live ? "true" : "false", st.hid_open_result, st.report_hz,
              st.suspended ? "true" : "false");
    jb_str(j, R.data_dir);
    static conf_warns_t w[CF_COUNT];
    pthread_mutex_lock(&R.snap_lock);
    memcpy(w, R.pub_warns, sizeof w);
    unsigned gen = R.pub_gen;
    int last = R.pub_last_reload;
    pthread_mutex_unlock(&R.snap_lock);
    jb_printf(j, ",\"gen\":%u,\"reloaded\":", gen);
    jb_str(j, last > 0 ? conf_schema((conf_file_t)(last - 1))->file : "");
    jb_printf(j, ",\"warnings\":[");
    int nw = 0, dropped = 0;
    for (int f = 0; f < CF_COUNT; f++) {
        dropped += w[f].dropped;
        for (int i = 0; i < w[f].n; i++) {
            jb_printf(j, "%s{\"file\":", nw++ ? "," : "");
            jb_str(j, conf_schema((conf_file_t)f)->file);
            jb_printf(j, ",\"line\":%d,\"key\":", w[f].w[i].line);
            jb_str(j, w[f].w[i].key);
            jb_printf(j, ",\"msg\":");
            jb_str(j, w[f].w[i].msg);
            jb_printf(j, "}");
        }
    }
    jb_printf(j, "],\"dropped\":%d}}", dropped);
}

void dsxp_api(const char *method, const char *path, const char *query, dsxp_response_t *out) {
    api_env_t env = { NULL, runtime_snapshot, runtime_submit, runtime_catalog, runtime_app_info };
    api_handle(&env, method, path, query, out);
}

void dsxp_response_free(dsxp_response_t *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
