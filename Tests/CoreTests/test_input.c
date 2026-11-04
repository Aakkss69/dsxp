/* Input side: report 0x31 decoding, stick shaping, touchpad gestures. */
#include "tests.h"
#include <string.h>

static void make_report(unsigned char *r, int lx, int ly, int rx, int ry, int l2, int r2,
                        int b0, int b1, int b2) {
    memset(r, 0, 78);
    r[0] = 0x31;
    r[OFF_LX] = (unsigned char)lx; r[OFF_LY] = (unsigned char)ly;
    r[OFF_RX] = (unsigned char)rx; r[OFF_RY] = (unsigned char)ry;
    r[OFF_L2] = (unsigned char)l2; r[OFF_R2] = (unsigned char)r2;
    r[OFF_BTN0] = (unsigned char)b0; r[OFF_BTN1] = (unsigned char)b1; r[OFF_BTN2] = (unsigned char)b2;
    r[OFF_TOUCH0] = 0x80;     /* not touching */
    r[OFF_TOUCH1] = 0x80;
}

static void set_touch(unsigned char *r, int which, bool on, unsigned x, unsigned y) {
    unsigned char *t = r + (which ? OFF_TOUCH1 : OFF_TOUCH0);
    t[0] = on ? 0x01 : 0x80;
    t[1] = (unsigned char)(x & 0xFF);
    t[2] = (unsigned char)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    t[3] = (unsigned char)(y >> 4);
}

void test_decode(void) {
    ds_state_t st; gesture_t g; unsigned char r[78];
    tuning_t tz = { 0.0, 1.0, 0.5 };
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);

    make_report(r, 128, 0, 255, 128, 0, 255, 0x08, 0, 0);
    CHECK(ds_decode_report(&st, &g, &tz, r, 78, 1.0));
    CHECK_NEAR(st.val[S_LX], 0.0, 1e-9);
    CHECK_NEAR(st.val[S_LY], -1.0, 1e-9);           /* (0-128)/127 clamps to -1 */
    CHECK_NEAR(st.val[S_RX], 1.0, 1e-9);
    CHECK_NEAR(st.val[S_L2], 0.0, 1e-9);
    CHECK_NEAR(st.val[S_R2], 1.0, 1e-9);
    CHECK_NEAR(st.raw01[S_RX], 1.0, 1e-9);
    for (int i = 0; i < B_COUNT; i++) CHECK(!st.btn[i]);   /* dpad 8 = released */

    for (int d = 0; d < 8; d++) {
        make_report(r, 128, 128, 128, 128, 0, 0, d, 0, 0);
        ds_decode_report(&st, &g, &tz, r, 78, 1.0);
        for (int i = 0; i < 8; i++) CHECK(st.btn[i] == (i == d));
    }

    struct { int byte, bit, slot; } map[] = {
        { 0, 0x10, B_SQUARE }, { 0, 0x20, B_CROSS }, { 0, 0x40, B_CIRCLE }, { 0, 0x80, B_TRIANGLE },
        { 1, 0x01, B_L1 }, { 1, 0x02, B_R1 }, { 1, 0x04, B_L2D }, { 1, 0x08, B_R2D },
        { 1, 0x10, B_CREATE }, { 1, 0x20, B_OPTIONS }, { 1, 0x40, B_L3 }, { 1, 0x80, B_R3 },
        { 2, 0x01, B_PS }, { 2, 0x02, B_TOUCHPAD }, { 2, 0x04, B_MUTE },
    };
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
        int b[3] = { 0x08, 0, 0 };
        b[map[k].byte] |= map[k].bit;
        make_report(r, 128, 128, 128, 128, 0, 0, b[0], b[1], b[2]);
        ds_decode_report(&st, &g, &tz, r, 78, 1.0);
        for (int i = 0; i < B_TP_TAP; i++) CHECK(st.btn[i] == (i == map[k].slot));
    }

    /* every byte read lies below IN_REPORT_MIN_LEN; shorter reports are refused */
    make_report(r, 0, 0, 0, 0, 0, 0, 0x08, 0, 0);
    CHECK(!ds_decode_report(&st, &g, &tz, r, IN_REPORT_MIN_LEN - 1, 1.0));
    CHECK(ds_decode_report(&st, &g, &tz, r, IN_REPORT_MIN_LEN, 1.0));
    CHECK(!ds_decode_report(&st, &g, &tz, NULL, 78, 1.0));
    r[0] = 0x01;
    CHECK(!ds_decode_report(&st, &g, &tz, r, 78, 1.0));

    make_report(r, 128, 128, 128, 128, 0, 0, 0x08, 0, 0);
    set_touch(r, 0, true, 1919, 1079);
    set_touch(r, 1, true, 0, 0);
    ds_decode_report(&st, &g, &tz, r, 78, 1.0);
    CHECK(st.touch[0].active && st.touch[1].active);
    CHECK_NEAR(st.touch[0].x, 1919 / 1920.0, 1e-9);
    CHECK_NEAR(st.touch[0].y, 1079 / 1080.0, 1e-9);
    CHECK_NEAR(st.touch[1].x, 0.0, 1e-9);
}

void test_shape(void) {
    CHECK_NEAR(shape(0.03, 0.06, 1.0), 0.0, 1e-12);
    CHECK_NEAR(shape(-0.06, 0.06, 1.0), 0.0, 1e-12);
    CHECK_NEAR(shape(1.0, 0.06, 1.0), 1.0, 1e-12);
    CHECK_NEAR(shape(-1.0, 0.06, 1.0), -1.0, 1e-12);
    CHECK_NEAR(shape(5.0, 0.06, 1.0), 1.0, 1e-12);
    CHECK_NEAR(shape(0.06 + 1e-9, 0.06, 1.0), 0.0, 1e-6);   /* continuous at the edge */
    CHECK_NEAR(shape(0.5, 0.0, 2.0), 0.25, 1e-12);
    CHECK_NEAR(shape(-0.5, 0.0, 2.0), -0.25, 1e-12);
    CHECK_NEAR(shape(NAN, 0.06, 1.0), 0.0, 1e-12);
    CHECK_NEAR(shape(0.5, NAN, NAN), 0.5, 1e-12);
    CHECK_NEAR(shape(0.5, 0.0, -1.0), 0.5, 1e-12);          /* nonsense curve: linear */
    double prev = -2;
    for (int i = -100; i <= 100; i++) {
        double v = shape(i / 100.0, 0.1, 1.7);
        CHECK(v >= prev - 1e-12);
        prev = v;
    }
    /* the web UI's shaped() in app.js mirrors this; same vectors */
    CHECK_NEAR(shape(0.5, 0.1, 1.0), (0.5 - 0.1) / 0.9, 1e-12);
}

static void touch_at(ds_state_t *st, bool on, double x, double y) {
    st->touch[0].active = on;
    if (on) { st->touch[0].x = x; st->touch[0].y = y; }
}

static int fired(const gesture_t *g, double now) {
    int slot = -1;
    for (int i = B_TP_TAP; i < B_COUNT; i++) if (now < g->pulse[i]) slot = i;
    return slot;
}

static int swipe(double ang_deg, double dist, double sens) {
    ds_state_t st; gesture_t g;
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    double a = ang_deg * M_PI / 180.0;
    touch_at(&st, true, 0.5, 0.5);
    detect_gestures(&st, &g, sens, 10.0);
    touch_at(&st, true, 0.5 + dist * cos(a), 0.5 + dist * sin(a));
    detect_gestures(&st, &g, sens, 10.1);
    touch_at(&st, false, 0, 0);
    detect_gestures(&st, &g, sens, 10.15);
    return fired(&g, 10.16);
}

void test_gesture(void) {
    /* pad y grows downward: 90 degrees is a swipe DOWN */
    int expect[8] = { B_TP_SWIPE_R, B_TP_SWIPE_DR, B_TP_SWIPE_D, B_TP_SWIPE_DL,
                      B_TP_SWIPE_L, B_TP_SWIPE_UL, B_TP_SWIPE_U, B_TP_SWIPE_UR };
    for (int s = 0; s < 8; s++) CHECK_INT(swipe(s * 45.0, 0.35, 0.5), expect[s]);

    /* sensitivity: a 0.10 stroke is a swipe at 1.0 but nothing at 0.0 */
    CHECK_INT(swipe(0, 0.10, 1.0), B_TP_SWIPE_R);
    CHECK_INT(swipe(0, 0.10, 0.0), -1);

    ds_state_t st; gesture_t g;
    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    touch_at(&st, true, 0.4, 0.4); detect_gestures(&st, &g, 0.5, 1.0);
    touch_at(&st, false, 0, 0);    detect_gestures(&st, &g, 0.5, 1.1);
    CHECK_INT(fired(&g, 1.11), B_TP_TAP);
    CHECK_INT(fired(&g, 1.1 + GESTURE_PULSE_SEC + 0.001), -1);   /* pulse ends */

    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);
    touch_at(&st, true, 0.4, 0.4); st.touch[1].active = true;
    detect_gestures(&st, &g, 0.5, 2.0);
    st.touch[1].active = false;
    touch_at(&st, false, 0, 0);    detect_gestures(&st, &g, 0.5, 2.1);
    CHECK_INT(fired(&g, 2.11), B_TP_TAP2);

    memset(&st, 0, sizeof st); memset(&g, 0, sizeof g);   /* long press: nothing */
    touch_at(&st, true, 0.4, 0.4); detect_gestures(&st, &g, 0.5, 3.0);
    touch_at(&st, false, 0, 0);    detect_gestures(&st, &g, 0.5, 4.0);
    CHECK_INT(fired(&g, 4.01), -1);
}
