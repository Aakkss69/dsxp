/* HID plumbing: open the Bluetooth DualSense and decode input report 0x31. */
#include "dsxp.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned char g_buf[512];

/* A gesture is an event, not a state, so it is published as a short pulse on its
 * slot. That lets xp_push see a clean press edge with no special-casing. */
#define PULSE_SEC 0.08
static double g_pulse[B_COUNT];
static void fire(int slot) { g_pulse[slot] = now_sec() + PULSE_SEC; }

static void decode_touch(const unsigned char *r) {
    for (int i = 0; i < 2; i++) {
        const unsigned char *t = r + (i ? OFF_TOUCH1 : OFF_TOUCH0);
        bool active = (t[0] & 0x80) == 0;            /* bit 7 set means lifted */
        g_state.touch[i].active = active;
        if (!active) continue;
        unsigned x = t[1] | ((unsigned)(t[2] & 0x0F) << 8);
        unsigned y = ((t[2] & 0xF0) >> 4) | ((unsigned)t[3] << 4);
        g_state.touch[i].x = x / TOUCH_W;
        g_state.touch[i].y = y / TOUCH_H;
    }
}

/* Tap vs swipe, decided on release. Sensitivity shortens the swipe a gesture
 * needs to travel: 0.0 wants a deliberate stroke, 1.0 triggers on a flick. */
static void detect_gestures(void) {
    static bool was = false, was2 = false;
    static double sx, sy, st;
    static bool saw_second;

    bool now1 = g_state.touch[0].active;
    bool now2 = g_state.touch[1].active;
    double t = now_sec();

    if (now1 && !was) {
        sx = g_state.touch[0].x; sy = g_state.touch[0].y; st = t;
        saw_second = now2;
    }
    if (now1 && now2) saw_second = true;

    if (!now1 && was) {
        double dx = g_state.touch[0].x - sx;
        double dy = g_state.touch[0].y - sy;
        double dist = sqrt(dx * dx + dy * dy);
        double dt = t - st;
        double sens = g_tune.gesture;
        if (sens < 0) sens = 0;
        if (sens > 1) sens = 1;
        double swipe_min = 0.30 - 0.22 * sens;
        double tap_max   = 0.04 + 0.03 * (1.0 - sens);

        if (dist >= swipe_min) {
            /* eight 45-degree sectors, starting at "right" and going clockwise
             * (screen y grows downward, so positive dy is down) */
            static const int SECTOR[8] = {
                B_TP_SWIPE_R,  B_TP_SWIPE_DR, B_TP_SWIPE_D,  B_TP_SWIPE_DL,
                B_TP_SWIPE_L,  B_TP_SWIPE_UL, B_TP_SWIPE_U,  B_TP_SWIPE_UR,
            };
            double deg = atan2(dy, dx) * 180.0 / M_PI;
            if (deg < 0) deg += 360.0;
            int sec = ((int)((deg + 22.5) / 45.0)) & 7;
            fire(SECTOR[sec]);
        } else if (dist <= tap_max && dt <= 0.35) {
            fire(saw_second ? B_TP_TAP2 : B_TP_TAP);
        }
        saw_second = false;
    }
    was = now1; was2 = now2;
    (void)was2;
}

/* Deadzone then optional power curve. Both live in g_tune and are adjustable
 * from the UI at runtime, because the override datarefs bypass X-Plane's own
 * null zone and response curves - nothing downstream will filter stick drift. */
static double shape(double v) {
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    double dz = g_tune.deadzone;
    if (dz > 0.0) {
        double a = fabs(v);
        if (a <= dz) return 0.0;
        v = (v > 0 ? 1.0 : -1.0) * (a - dz) / (1.0 - dz);
    }
    if (g_tune.curve != 1.0) {
        double a = pow(fabs(v), g_tune.curve);
        v = (v > 0 ? a : -a);
    }
    return v;
}

void ds_decode_report(const unsigned char *r, long len) {
    if (len < 13) return;
    static const int off[S_COUNT] = { OFF_LX, OFF_LY, OFF_RX, OFF_RY, OFF_L2, OFF_R2 };

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < S_COUNT; i++) {
        unsigned char raw = r[off[i]];
        g_state.raw01[i] = (double)raw / 255.0;
        /* triggers are unipolar 0..1; sticks are centred at 128 */
        g_state.val[i] = (i >= S_L2) ? g_state.raw01[i]
                                     : shape(((double)raw - 128.0) / 127.0);
    }

    for (int i = 0; i < B_COUNT; i++) g_state.btn[i] = false;
    unsigned dpad = r[OFF_BTN0] & 0x0F;      /* 0..7 = direction, 8 = released */
    if (dpad <= 7) g_state.btn[dpad] = true;

    unsigned char b0 = r[OFF_BTN0], b1 = r[OFF_BTN1], b2 = r[OFF_BTN2];
    g_state.btn[B_SQUARE]   = (b0 & 0x10) != 0;
    g_state.btn[B_CROSS]    = (b0 & 0x20) != 0;
    g_state.btn[B_CIRCLE]   = (b0 & 0x40) != 0;
    g_state.btn[B_TRIANGLE] = (b0 & 0x80) != 0;
    g_state.btn[B_L1]       = (b1 & 0x01) != 0;
    g_state.btn[B_R1]       = (b1 & 0x02) != 0;
    g_state.btn[B_L2D]      = (b1 & 0x04) != 0;
    g_state.btn[B_R2D]      = (b1 & 0x08) != 0;
    g_state.btn[B_CREATE]   = (b1 & 0x10) != 0;
    g_state.btn[B_OPTIONS]  = (b1 & 0x20) != 0;
    g_state.btn[B_L3]       = (b1 & 0x40) != 0;
    g_state.btn[B_R3]       = (b1 & 0x80) != 0;
    g_state.btn[B_PS]       = (b2 & 0x01) != 0;
    g_state.btn[B_TOUCHPAD] = (b2 & 0x02) != 0;
    g_state.btn[B_MUTE]     = (b2 & 0x04) != 0;

    decode_touch(r);
    detect_gestures();
    double tnow = now_sec();
    for (int i = B_TP_TAP; i < B_COUNT; i++)
        g_state.btn[i] = tnow < g_pulse[i];

    g_have_report = true;
    g_reports++;
    pthread_mutex_unlock(&g_lock);
}

static void report_cb(void *ctx, IOReturn res, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex len) {
    (void)ctx; (void)res; (void)sender; (void)type;
    if (reportID != BT_REPORT_ID) return;
    ds_decode_report(report, (long)len);
}

static void device_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    (void)ctx; (void)res; (void)sender;
    CFStringRef tr = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));
    char tb[64] = "?";
    if (tr) CFStringGetCString(tr, tb, sizeof tb, kCFStringEncodingUTF8);
    if (strcmp(tb, "Bluetooth") != 0) {
        fprintf(stderr, "[dsxp] DualSense on %s - ignoring (USB works natively)\n", tb);
        return;
    }
    fprintf(stderr, "[dsxp] DualSense on Bluetooth - listening for report 0x%02X\n",
            BT_REPORT_ID);
    g_dev = dev;          /* keep it for output reports: light bar, triggers */
    IOHIDDeviceRegisterInputReportCallback(dev, g_buf, sizeof g_buf, report_cb, NULL);
}

static void removal_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    (void)ctx; (void)res; (void)sender; (void)dev;
    g_have_report = false;
    g_dev = NULL;
    fprintf(stderr, "[dsxp] DualSense removed\n");
}

IOHIDManagerRef ds_open(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = DS_VID, pid = DS_PID;
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef vn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey),  vn);
    CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), pn);
    IOHIDManagerSetDeviceMatching(mgr, m);
    CFRelease(vn); CFRelease(pn); CFRelease(m);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_cb,  NULL);
    IOHIDManagerRegisterDeviceRemovalCallback (mgr, removal_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "[dsxp] IOHIDManagerOpen failed 0x%08x - grant Input Monitoring "
                        "in System Settings > Privacy & Security\n", r);
        return NULL;
    }
    return mgr;
}
