/*
 * X-Plane side: override handling, per-frame push of axes and commands.
 * Pure: packets go out through c->sinks.udp_send.
 */
#include "core_internal.h"
#include <math.h>
#include <string.h>

/* Writable datarefs, confirmed against X-Plane 12 Resources/plugins/DataRefs.txt
 * (writable column = "y"). */
#define DR_PITCH   "sim/joystick/yoke_pitch_ratio"
#define DR_ROLL    "sim/joystick/yoke_roll_ratio"
#define DR_HEADING "sim/joystick/yoke_heading_ratio"
#define DR_THROTL  "sim/cockpit2/engine/actuators/throttle_ratio_all"
#define DR_LBRAKE  "sim/cockpit2/controls/left_brake_ratio"
#define DR_RBRAKE  "sim/cockpit2/controls/right_brake_ratio"

static const char *OV_PATH[OV_COUNT] = {
    "sim/operation/override/override_joystick_pitch",
    "sim/operation/override/override_joystick_roll",
    "sim/operation/override/override_joystick_heading",
    "sim/operation/override/override_toe_brakes",
};

void xp_send_dref(core_t *c, const char *path, float value) {
    if (!c->sinks.udp_send) return;
    unsigned char p[DREF_LEN];
    size_t n = xp_build_dref(p, path, value);
    c->sinks.udp_send(c->sinks.ctx, p, n);
}

void xp_send_cmnd(core_t *c, const char *cmd) {
    if (!c->sinks.udp_send) return;
    unsigned char p[CMND_MAX];
    size_t n = xp_build_cmnd(p, cmd);
    c->sinks.udp_send(c->sinks.ctx, p, n);
}

bool xp_axis_in_use(const settings_t *s, int target) {
    for (int i = 0; i < S_COUNT; i++) if (s->axis[i].target == target) return true;
    return false;
}

/* Only the overrides an axis actually needs, so anything unmapped stays under
 * X-Plane's own control instead of being frozen. */
void xp_wanted_overrides(const settings_t *s, bool want[OV_COUNT]) {
    want[OV_PITCH_I]   = xp_axis_in_use(s, AX_PITCH);
    want[OV_ROLL_I]    = xp_axis_in_use(s, AX_ROLL);
    want[OV_HEADING_I] = xp_axis_in_use(s, AX_YAW) || xp_axis_in_use(s, AX_RUDD_L) ||
                         xp_axis_in_use(s, AX_RUDD_R);
    want[OV_BRAKES_I]  = xp_axis_in_use(s, AX_BRAKE_L) || xp_axis_in_use(s, AX_BRAKE_R);
}

/* Neutral value for the control an override covers, sent before releasing it. */
static void center_for(core_t *c, int ov) {
    switch (ov) {
    case OV_PITCH_I:   xp_send_dref(c, DR_PITCH, 0.0f);   break;
    case OV_ROLL_I:    xp_send_dref(c, DR_ROLL, 0.0f);    break;
    case OV_HEADING_I: xp_send_dref(c, DR_HEADING, 0.0f); break;
    case OV_BRAKES_I:  xp_send_dref(c, DR_LBRAKE, 0.0f);
                       xp_send_dref(c, DR_RBRAKE, 0.0f);  break;
    default: break;
    }
}

void xp_engage(core_t *c, double now) {
    (void)now;
    if (c->engaged || c->dry_run) return;
    c->engaged = true;
    memset(c->ov_on, 0, sizeof c->ov_on);
    memset(c->ov_release_left, 0, sizeof c->ov_release_left);
    memset(c->prev_btn, 0, sizeof c->prev_btn);
    dsxp_log("ENGAGED - streaming to %s:%d", XP_IP, XP_PORT);
    /* the overrides themselves are asserted by xp_push's planner, every tick */
}

/*
 * Center the controls (toe brakes too) and release every override, twice: UDP can
 * drop. If X-Plane was busy (loading, stalled) its socket buffer may be full and
 * drop exactly these packets, so every override that was on gets further releases
 * from xp_release_tick() while X-Plane keeps running.
 */
void xp_disengage(core_t *c, double now) {
    if (!c->engaged) return;
    for (int k = 0; k < 2; k++) {
        xp_send_dref(c, DR_PITCH, 0.0f);
        xp_send_dref(c, DR_ROLL, 0.0f);
        xp_send_dref(c, DR_HEADING, 0.0f);
        xp_send_dref(c, DR_LBRAKE, 0.0f);
        xp_send_dref(c, DR_RBRAKE, 0.0f);
        for (int i = 0; i < OV_COUNT; i++) xp_send_dref(c, OV_PATH[i], 0.0f);
    }
    for (int i = 0; i < OV_COUNT; i++) {
        if (c->ov_on[i]) {
            c->ov_release_left[i] = c->bridge.override_release_repeats;
            c->ov_release_at[i] = now + c->bridge.override_reassert_s;
        }
        c->ov_on[i] = false;
    }
    c->engaged = false;
    c->thr_sent = false;
    memset(c->prev_btn, 0, sizeof c->prev_btn);
    dsxp_log("DISENGAGED - X-Plane has controls back");
}

/* Further override releases after a disengage, while X-Plane is still running. */
void xp_release_tick(core_t *c, double now) {
    if (c->engaged || c->dry_run) return;
    if (!c->xp_running) {                    /* X-Plane gone: its overrides went with it */
        memset(c->ov_release_left, 0, sizeof c->ov_release_left);
        return;
    }
    for (int k = 0; k < OV_COUNT; k++) {
        if (c->ov_release_left[k] > 0 && now >= c->ov_release_at[k]) {
            center_for(c, k);
            xp_send_dref(c, OV_PATH[k], 0.0f);
            c->ov_release_left[k]--;
            c->ov_release_at[k] = now + c->bridge.override_reassert_s;
        }
    }
}

/*
 * Override planner. A wanted override is re-asserted every second: X-Plane may
 * not be listening yet when the process first appears, and it resets overrides
 * on an aircraft reload. An override that stops being wanted (its axis was
 * unmapped) gets a centered value, then "off", repeated a few times, so the axis
 * goes straight back to X-Plane instead of freezing where it was.
 */
static void plan_overrides(core_t *c, double now) {
    bool want[OV_COUNT];
    xp_wanted_overrides(&c->s, want);
    for (int k = 0; k < OV_COUNT; k++) {
        if (want[k]) {
            if (!c->ov_on[k] || now - c->ov_last[k] >= c->bridge.override_reassert_s) {
                xp_send_dref(c, OV_PATH[k], 1.0f);
                c->ov_on[k] = true;
                c->ov_last[k] = now;
                c->ov_release_left[k] = 0;
            }
        } else if (c->ov_on[k]) {
            center_for(c, k);
            xp_send_dref(c, OV_PATH[k], 0.0f);
            c->ov_on[k] = false;
            c->ov_release_left[k] = c->bridge.override_release_repeats;
            c->ov_release_at[k] = now + c->bridge.override_reassert_s;
        } else if (c->ov_release_left[k] > 0 && now >= c->ov_release_at[k]) {
            xp_send_dref(c, OV_PATH[k], 0.0f);
            c->ov_release_left[k]--;
            c->ov_release_at[k] = now + c->bridge.override_reassert_s;
        }
    }
}

void xp_push(core_t *c, double now) {
    if (!c->engaged) return;
    plan_overrides(c, now);

    const settings_t *s = &c->s;
    /* No fresh input (Bluetooth hiccup, controller switched off): hold every
     * axis neutral and release every button rather than replaying stale values. */
    bool live = c->have_report && (now - c->last_report_at) <= c->bridge.input_stale_s;

    double roll = 0, pitch = 0, yaw = 0, thr = -1, lb = -1, rb = -1;
    if (live) {
        for (int i = 0; i < S_COUNT; i++) {
            double v = c->state.val[i];       /* -1..1 for sticks, 0..1 for triggers */
            double u = c->state.raw01[i];     /* always 0..1                          */
            if (s->axis[i].invert) { v = -v; u = 1.0 - u; }
            /* a stick used as a unipolar control needs its centered value remapped */
            double uni = (i >= S_L2) ? u : (v + 1.0) / 2.0;
            switch (s->axis[i].target) {
            case AX_ROLL:     roll  += v;   break;
            case AX_PITCH:    pitch += v;   break;
            case AX_YAW:      yaw   += v;   break;
            case AX_THROTTLE: thr = (i >= S_L2) ? u : (1.0 - u); break;
            case AX_RUDD_L:   yaw -= uni;   break;
            case AX_RUDD_R:   yaw += uni;   break;
            case AX_BRAKE_L:  lb   = uni;   break;
            case AX_BRAKE_R:  rb   = uni;   break;
            default: break;
            }
        }
    } else {
        /* throttle is left where it is (thr stays -1); X-Plane holds the value */
        if (xp_axis_in_use(s, AX_BRAKE_L)) lb = 0;
        if (xp_axis_in_use(s, AX_BRAKE_R)) rb = 0;
    }
    if (roll  >  1) roll  =  1;
    if (roll  < -1) roll  = -1;
    if (pitch >  1) pitch =  1;
    if (pitch < -1) pitch = -1;
    if (yaw   >  1) yaw   =  1;
    if (yaw   < -1) yaw   = -1;

    bool want[OV_COUNT];
    xp_wanted_overrides(s, want);
    if (want[OV_ROLL_I])    xp_send_dref(c, DR_ROLL,    (float)roll);
    if (want[OV_PITCH_I])   xp_send_dref(c, DR_PITCH,   (float)pitch);
    if (want[OV_HEADING_I]) xp_send_dref(c, DR_HEADING, (float)yaw);
    /* Throttle has no override, so writing it every tick would fight the mouse,
     * keyboard and autothrottle. Send it on change, plus a slow keepalive. */
    if (thr >= 0 && (!c->thr_sent || fabs(thr - c->last_thr) >= 0.002 ||
                     (c->bridge.throttle_keepalive_s > 0 && now - c->last_thr_at >= c->bridge.throttle_keepalive_s))) {
        xp_send_dref(c, DR_THROTL, (float)thr);
        c->last_thr = thr;
        c->last_thr_at = now;
        c->thr_sent = true;
    }
    if (lb  >= 0) xp_send_dref(c, DR_LBRAKE, (float)lb);
    if (rb  >= 0) xp_send_dref(c, DR_RBRAKE, (float)rb);

    for (int i = 0; i < B_COUNT; i++) {
        bool down = live && c->state.btn[i];
        if (!s->bind[i].cmd[0]) { c->prev_btn[i] = down; continue; }
        bool was = c->prev_btn[i];
        if (down && !was) {
            xp_send_cmnd(c, s->bind[i].cmd);
            c->last_cmd_time[i] = now;
            if (c->verbose) dsxp_log("%s -> %s", BTN_NAME[i], s->bind[i].cmd);
        } else if (down && was && s->bind[i].repeat &&
                   now - c->last_cmd_time[i] >= 1.0 / c->bridge.repeat_hz) {
            xp_send_cmnd(c, s->bind[i].cmd);
            c->last_cmd_time[i] = now;
        }
        c->prev_btn[i] = down;
    }
}
