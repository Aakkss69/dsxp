/* Decode Bluetooth input report 0x31: sticks, triggers, buttons, touchpad. Pure. */
#include "core_internal.h"
#include <math.h>
#include <string.h>

/* Deadzone then optional power curve. Both are adjustable from the UI because
 * values written through the override datarefs are not filtered by X-Plane's
 * own null zone or response curves (observed, not documented by Laminar; see
 * docs/REFERENCES.md) - nothing downstream will filter stick drift. */
double shape(double v, double deadzone, double curve) {
    if (!isfinite(v)) return 0.0;
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    double dz = isfinite(deadzone) ? deadzone : 0.0;
    if (dz > 0.0 && dz < 1.0) {
        double a = fabs(v);
        if (a <= dz) return 0.0;
        v = (v > 0 ? 1.0 : -1.0) * (a - dz) / (1.0 - dz);
    }
    if (isfinite(curve) && curve > 0.0 && curve != 1.0) {
        double a = pow(fabs(v), curve);
        v = (v > 0 ? a : -a);
    }
    return v;
}

void decode_touch(ds_state_t *st, const unsigned char *r) {
    for (int i = 0; i < 2; i++) {
        const unsigned char *t = r + (i ? OFF_TOUCH1 : OFF_TOUCH0);
        bool active = (t[0] & 0x80) == 0;            /* bit 7 set means lifted */
        st->touch[i].active = active;
        if (!active) continue;                       /* keep last x/y for the release */
        unsigned x = t[1] | ((unsigned)(t[2] & 0x0F) << 8);
        unsigned y = ((t[2] & 0xF0) >> 4) | ((unsigned)t[3] << 4);
        st->touch[i].x = x / TOUCH_W;
        st->touch[i].y = y / TOUCH_H;
    }
}

/* A gesture is an event, not a state, so it is published as a short pulse on its
 * slot. That gives xp_push a clean press edge with no special-casing. */
static void fire(gesture_t *g, int slot, double pulse_s, double now) { g->pulse[slot] = now + pulse_s; }

/* Tap vs swipe, decided on release. Sensitivity shortens the swipe a gesture
 * needs to travel: 0.0 wants a deliberate stroke, 1.0 triggers on a flick. */
void detect_gestures(const ds_state_t *st, gesture_t *g, double sens, double now) {
    detect_gestures_cfg(st, g, sens, NULL, now);
}

void detect_gestures_cfg(const ds_state_t *st, gesture_t *g, double sens,
                         const gesture_cfg_t *gc, double now) {
    if (!gc) gc = &GESTURE_DEFAULTS;
    bool now1 = st->touch[0].active;
    bool now2 = st->touch[1].active;

    if (now1 && !g->was) {
        g->sx = st->touch[0].x; g->sy = st->touch[0].y; g->st = now;
        g->saw_second = now2;
    }
    if (now1 && now2) g->saw_second = true;

    if (!now1 && g->was) {
        double dx = st->touch[0].x - g->sx;
        double dy = st->touch[0].y - g->sy;
        double dist = sqrt(dx * dx + dy * dy);
        double dt = now - g->st;
        if (!isfinite(sens) || sens < 0) sens = 0;
        if (sens > 1) sens = 1;
        double swipe_min = gc->swipe_at0 + (gc->swipe_at100 - gc->swipe_at0) * sens;
        double tap_max   = gc->wander_at0 + (gc->wander_at100 - gc->wander_at0) * sens;

        if (dist >= swipe_min) {
            /* eight 45-degree sectors, starting at "right" and going clockwise
             * (pad y grows downward, so positive dy is down) */
            static const int SECTOR[8] = {
                B_TP_SWIPE_R,  B_TP_SWIPE_DR, B_TP_SWIPE_D,  B_TP_SWIPE_DL,
                B_TP_SWIPE_L,  B_TP_SWIPE_UL, B_TP_SWIPE_U,  B_TP_SWIPE_UR,
            };
            double deg = atan2(dy, dx) * 180.0 / M_PI;
            if (deg < 0) deg += 360.0;
            int sec = ((int)((deg + 22.5) / 45.0)) & 7;
            fire(g, SECTOR[sec], gc->pulse_s, now);
        } else if (dist <= tap_max && dt <= gc->tap_time_s) {
            fire(g, g->saw_second ? B_TP_TAP2 : B_TP_TAP, gc->pulse_s, now);
        }
        g->saw_second = false;
    }
    g->was = now1;
}

bool ds_decode_report(ds_state_t *st, gesture_t *g, const tuning_t *tune,
                      const unsigned char *r, long len, double now) {
    return ds_decode_report_cfg(st, g, tune, NULL, r, len, now);
}

bool ds_decode_report_cfg(ds_state_t *st, gesture_t *g, const tuning_t *tune,
                          const gesture_cfg_t *gc, const unsigned char *r, long len, double now) {
    if (!r || len < IN_REPORT_MIN_LEN || r[0] != BT_REPORT_ID) return false;
    static const int off[S_COUNT] = { OFF_LX, OFF_LY, OFF_RX, OFF_RY, OFF_L2, OFF_R2 };

    for (int i = 0; i < S_COUNT; i++) {
        unsigned char raw = r[off[i]];
        st->raw01[i] = (double)raw / 255.0;
        /* triggers are unipolar 0..1; sticks are centered at 128 */
        st->val[i] = (i >= S_L2) ? st->raw01[i]
                                 : shape(((double)raw - 128.0) / 127.0,
                                         tune->deadzone, tune->curve);
    }

    for (int i = 0; i < B_COUNT; i++) st->btn[i] = false;
    unsigned dpad = r[OFF_BTN0] & 0x0F;      /* 0..7 = direction, 8 = released */
    if (dpad <= 7) st->btn[dpad] = true;

    unsigned char b0 = r[OFF_BTN0], b1 = r[OFF_BTN1], b2 = r[OFF_BTN2];
    st->btn[B_SQUARE]   = (b0 & 0x10) != 0;
    st->btn[B_CROSS]    = (b0 & 0x20) != 0;
    st->btn[B_CIRCLE]   = (b0 & 0x40) != 0;
    st->btn[B_TRIANGLE] = (b0 & 0x80) != 0;
    st->btn[B_L1]       = (b1 & 0x01) != 0;
    st->btn[B_R1]       = (b1 & 0x02) != 0;
    st->btn[B_L2D]      = (b1 & 0x04) != 0;
    st->btn[B_R2D]      = (b1 & 0x08) != 0;
    st->btn[B_CREATE]   = (b1 & 0x10) != 0;
    st->btn[B_OPTIONS]  = (b1 & 0x20) != 0;
    st->btn[B_L3]       = (b1 & 0x40) != 0;
    st->btn[B_R3]       = (b1 & 0x80) != 0;
    st->btn[B_PS]       = (b2 & 0x01) != 0;
    st->btn[B_TOUCHPAD] = (b2 & 0x02) != 0;
    st->btn[B_MUTE]     = (b2 & 0x04) != 0;

    decode_touch(st, r);
    detect_gestures_cfg(st, g, tune->gesture, gc, now);
    for (int i = B_TP_TAP; i < B_COUNT; i++)
        st->btn[i] = now < g->pulse[i];
    return true;
}
