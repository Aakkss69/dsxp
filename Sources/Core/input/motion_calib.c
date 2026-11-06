/*
 * Motion control calibration: a short guided routine the mapper requires before motion
 * control can be switched on (and that can be run again at any time). Pure: it reads
 * the fused motion state and each calibrated sample, and changes only the fusion's bias
 * and level so the user sees the result live. The core copies the results into the
 * settings when the routine reaches CALIB_DONE (core.c core_calib_finish).
 *
 * Steps (each completes by itself once the controller shows it; nothing to click):
 *   FLAT   face up on a table, untouched, 3 s: the gyro's resting drift (bias) and a
 *          check that the accelerometer reads 1 g (a warning if not, kept until the end)
 *   LEVEL  picked up, then held the way you fly, steady, 2 s: the level attitude
 *   ROLL   bank left and right as far as comfortable, then back to level
 *   PITCH  top edge up and down as far as comfortable, then back to level
 *   YAW    twist left and right as far as comfortable, then back to center
 * Each reach sets that axis's full-deflection angle to 90 % of the smaller side, so full
 * control is reachable without straining.
 *
 * Every threshold below comes from measurements of a real controller over Bluetooth on
 * this Mac (2026-11-07, read-only probe, 64 samples a second): see each define.
 *
 * DSXP silences its own rumble and vibrate-trigger effects while the routine runs (core.c
 * out_send), because they shake the gyro; FLAT and LEVEL also wait out the half second the
 * fusion keeps "quiet" after any vibration (motion.c, motion_t.quiet_until).
 */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* FLAT: on a table every axis stayed within 2.5 deg/s peak-to-peak over each 3 s stretch
 * (137 of 137 stretches; noise 1 sd 0.06 / 0.015 / 0.045 deg/s), while the stillest 3 s
 * held in a hand spanned 4.0 deg/s. So 2.5 tells "lying still" from "held". */
#define FLAT_STILL_P2P_DPS 2.5
#define FLAT_SECONDS       3.0
#define FLAT_FACE_UP       0.94   /* cos 20 deg: lying face up it rests on its grips, ~6 deg nose up */
#define FLAT_G_TOLERANCE   0.05   /* gravity measured 0.982 g; outside 1 +- 0.05 the scale is suspect */

/* LEVEL: held in a hand, steady, the gyro stayed under 3.2 deg/s (1 sd 0.4-0.8); 8 deg/s
 * allows a relaxed grip but not a deliberate movement. */
#define LEVEL_STEADY_DPS   8.0
#define LEVEL_SECONDS      2.0
/* LEVEL waits until the controller has been picked up, or a controller left lying on the
 * table (steady too) would be taken as the level. Any one of three signs counts, so a
 * gentle lift is caught as surely as a quick one:
 *   a turn     setting it down measured 56-104 deg/s, the table's noise under 0.3 deg/s:
 *              one sample over PICKUP_DPS means a hand moved it
 *   a tilt     the attitude has moved PICKUP_TILT_DEG from where it lay (it rests ~6 deg
 *              nose up on its grips; held to fly it is rarely within 3 deg of that)
 *   a tremor   a hand never holds it as still as a table: the bias-removed rate averaged
 *              ~0.07 deg/s on the table (1 sd 0.06 / 0.015 / 0.045 per axis) and ~0.6-1.3
 *              held (1 sd 0.4-0.8 per axis). Its average over TREMOR_TAU_S passing
 *              PICKUP_TREMOR_DPS (about the geometric mean) means a hand holds it. Each
 *              sample counts at most TREMOR_CLIP_DPS, so one knock on the table does not. */
#define PICKUP_DPS        20.0
#define PICKUP_TILT_DEG    3.0
#define PICKUP_TREMOR_DPS  0.3
#define TREMOR_TAU_S       0.5
#define TREMOR_CLIP_DPS    2.0

/* Reaches. Each side must reach at least this far (smaller would make an axis twitchy
 * against its 2-4 deg deadzone); back within CENTER_DEG of level for CENTER_SECONDS ends
 * the step; the full-deflection angle is REACH_SHARE of the smaller side. */
#define ROLL_MIN_DEG      15.0
#define PITCH_MIN_DEG     10.0
#define YAW_MIN_DEG       10.0
#define CENTER_DEG         6.0
#define CENTER_SECONDS     0.75
#define REACH_SHARE        0.9
#define PITCH_LIMIT_DEG   65.0    /* level pitch + full pitch stays 5 deg inside motion.c's 70 */
/* A level pitched further than this leaves less than the smallest full-deflection pitch
 * (MOTION_PITCH_FULL_MIN) before PITCH_LIMIT_DEG, so LEVEL refuses it (55 deg). */
#define LEVEL_PITCH_MAX   (PITCH_LIMIT_DEG - MOTION_PITCH_FULL_MIN)

static const char *const NAMES[CALIB_STEPS] = {
    "idle", "flat", "level", "roll", "pitch", "yaw", "done" };

const char *calib_step_name(int step) {
    return step >= 0 && step < CALIB_STEPS ? NAMES[step] : "idle";
}

/* Set up step `step` from scratch. */
static void enter(calib_t *c, int step) {
    c->step = step;
    c->hold_s = 0;
    c->n = 0;
    c->angle = c->lo = c->hi = 0;
    c->twist = 0;
    c->reached = false;
    c->picked_up = false;
    c->tremor = 0;
    c->note[0] = 0;
    c->need_s = step == CALIB_FLAT ? FLAT_SECONDS : step == CALIB_LEVEL ? LEVEL_SECONDS : 0;
    c->min_reach = step == CALIB_ROLL ? ROLL_MIN_DEG : step == CALIB_PITCH ? PITCH_MIN_DEG
                 : step == CALIB_YAW ? YAW_MIN_DEG : 0;
}

/* Begin (or begin again) at the first step; earlier results (and the warning) are
 * discarded. */
void calib_start(calib_t *c) {
    memset(c, 0, sizeof *c);
    enter(c, CALIB_FLAT);
}

/* Stop without results. */
void calib_cancel(calib_t *c) {
    memset(c, 0, sizeof *c);                                 /* step = CALIB_IDLE         */
}

static void say(calib_t *c, const char *text) {
    snprintf(c->note, sizeof c->note, "%s", text);
}

/* ------------------------------------------------------------------- steps */

/* FLAT: a still streak of FLAT_SECONDS, face up. Moved or turned over: start over. */
static void step_flat(calib_t *c, motion_t *m, const motion_sample_t *s, double dt) {
    if (m->t < m->quiet_until) {                               /* DSXP vibrated it just now  */
        say(c, "Waiting for the controller to stop vibrating.");
        c->n = 0; c->hold_s = 0;
        return;
    }
    double a = sqrt(s->acc_g[0] * s->acc_g[0] + s->acc_g[1] * s->acc_g[1] + s->acc_g[2] * s->acc_g[2]);
    bool face_up = a > 0.5 && s->acc_g[1] / a >= FLAT_FACE_UP;    /* gravity through the face */
    bool still = true;
    if (c->n == 0) {                                           /* a new streak starts here   */
        for (int i = 0; i < 3; i++) {
            c->min_g[i] = c->max_g[i] = s->gyro_dps[i];
            c->sum_g[i] = c->sum_a[i] = 0;
        }
    }
    for (int i = 0; i < 3; i++) {                              /* widen the streak's range   */
        if (s->gyro_dps[i] < c->min_g[i]) c->min_g[i] = s->gyro_dps[i];
        if (s->gyro_dps[i] > c->max_g[i]) c->max_g[i] = s->gyro_dps[i];
        if (c->max_g[i] - c->min_g[i] > FLAT_STILL_P2P_DPS) still = false;
        /* a steady turn has no spread, but no resting drift is this large either */
        if (fabs(s->gyro_dps[i]) > MOTION_BIAS_MAX_DPS) still = false;
    }
    if (!face_up) { say(c, "Lay it face up on a flat surface."); c->n = 0; c->hold_s = 0; return; }
    if (!still)   { say(c, "Keep your hands off it until the bar is full."); c->n = 0; c->hold_s = 0; return; }
    for (int i = 0; i < 3; i++) { c->sum_g[i] += s->gyro_dps[i]; c->sum_a[i] += s->acc_g[i]; }
    c->n++;
    c->hold_s += dt;
    c->note[0] = 0;
    if (c->hold_s < FLAT_SECONDS) return;                      /* not long enough yet        */

    double ax = c->sum_a[0] / c->n, ay = c->sum_a[1] / c->n, az = c->sum_a[2] / c->n;
    c->acc_g = sqrt(ax * ax + ay * ay + az * az);              /* gravity as measured        */
    for (int i = 0; i < 3; i++) {
        c->bias[i] = c->sum_g[i] / c->n;                       /* the resting drift          */
        if (fabs(c->bias[i]) > MOTION_BIAS_MAX_DPS) {          /* not a drift: a fault       */
            say(c, "The gyro drifts far more than it should. Leave it still a moment longer.");
            c->n = 0; c->hold_s = 0;
            return;
        }
    }
    memcpy(m->bias, c->bias, sizeof m->bias);                  /* use it from now on         */
    m->bias_learned = true;
    memcpy(c->rest_up, m->up, sizeof c->rest_up);              /* how it lies: LEVEL's "not
                                                                  picked up yet" reference   */
    enter(c, CALIB_LEVEL);
    /* worth knowing, not fatal: kept in `warning`, which no later step overwrites */
    if (fabs(c->acc_g - 1.0) > FLAT_G_TOLERANCE)
        snprintf(c->warning, sizeof c->warning, "Gravity reads %.2f g lying still (1.00 expected); "
                 "the controller's own calibration may be off.", c->acc_g);
}

/* True once something shows a hand holds the controller (see PICKUP_DPS above): a
 * turn, a tilt away from how it lay, or a hand's tremor. `w`: rotation rate, bias removed. */
static bool hand_holds(calib_t *c, const motion_t *m, double w, double dt) {
    double k = fmin(1.0, dt / TREMOR_TAU_S);                   /* share of the gap closed now */
    c->tremor += k * (fmin(w, TREMOR_CLIP_DPS) - c->tremor);   /* running average of |rate|  */
    double along = c->rest_up[0] * m->up[0] + c->rest_up[1] * m->up[1] + c->rest_up[2] * m->up[2];
    return w > PICKUP_DPS                                      /* turned                     */
        || along < cos(PICKUP_TILT_DEG * M_PI / 180.0)         /* tilted from where it lay   */
        || c->tremor > PICKUP_TREMOR_DPS;                      /* a hand's tremor            */
}

/* LEVEL: steady for LEVEL_SECONDS, face roughly up; the average "up" becomes level. */
static void step_level(calib_t *c, motion_t *m, const motion_sample_t *s, double dt) {
    if (m->t < m->quiet_until) return;                         /* DSXP vibrated it just now  */
    double w = 0;                                              /* rotation rate, bias removed */
    for (int i = 0; i < 3; i++) w += (s->gyro_dps[i] - m->bias[i]) * (s->gyro_dps[i] - m->bias[i]);
    w = sqrt(w);
    if (!c->picked_up && hand_holds(c, m, w, dt)) c->picked_up = true;   /* stays true     */
    if (!c->picked_up) { say(c, "Pick it up and hold it the way you fly."); return; }
    double bank, pitch;
    motion_attitude(m->up, &bank, &pitch);                     /* absolute attitude now      */
    if (m->out_of_range || fabs(bank) > MOTION_LEVEL_MAX_DEG || fabs(pitch) > MOTION_LEVEL_MAX_DEG) {
        say(c, "Hold it face up, the way you fly level.");
        c->n = 0; c->hold_s = 0;
        return;
    }
    if (fabs(pitch) > LEVEL_PITCH_MAX) {                       /* no room left to pitch      */
        say(c, "Hold it a little flatter: tilted this far there is no room left to pitch.");
        c->n = 0; c->hold_s = 0;
        return;
    }
    if (w > LEVEL_STEADY_DPS) {
        say(c, "Hold it steady.");
        c->n = 0; c->hold_s = 0;
        return;
    }
    if (c->n == 0) c->sum_up[0] = c->sum_up[1] = c->sum_up[2] = 0;
    for (int i = 0; i < 3; i++) c->sum_up[i] += m->up[i];      /* average the filtered "up"  */
    c->n++;
    c->hold_s += dt;
    c->note[0] = 0;
    if (c->hold_s < LEVEL_SECONDS) return;
    double up[3] = { c->sum_up[0] / c->n, c->sum_up[1] / c->n, c->sum_up[2] / c->n };
    motion_attitude(up, &c->level_bank, &c->level_pitch);
    motion_set_level(m, c->level_bank, c->level_pitch);        /* angles are from here now   */
    enter(c, CALIB_ROLL);
}

/* ROLL, PITCH, YAW: record the furthest each way; once both sides are far enough,
 * coming back to center for CENTER_SECONDS finishes the step. */
static void step_reach(calib_t *c, motion_t *m, const motion_sample_t *s, double dt, bool saturated) {
    if (saturated) {                                           /* past what the sensor reads */
        enter(c, c->step);
        say(c, "That was faster than the sensor can measure. Once more, a little slower.");
        return;
    }
    if (c->step == CALIB_YAW) {
        /* the twist about the controller's face (the same rate flight uses,
         * motion_yaw.c), added up plainly: no hold, so the reach is the hand's own */
        c->twist += motion_yaw_rate(s->gyro_dps, m->bias) * dt;   /* right is positive    */
        c->angle = c->twist;
    } else {
        c->angle = c->step == CALIB_ROLL ? m->bank_deg : m->pitch_deg;   /* relative to level */
    }
    if (m->out_of_range) {                                     /* face down or on its edge   */
        say(c, "Too far: the controller can't be read past that. Come back a little.");
        c->hold_s = 0;
        return;
    }
    if (c->angle < c->lo) c->lo = c->angle;
    if (c->angle > c->hi) c->hi = c->angle;
    c->reached = c->hi >= c->min_reach && -c->lo >= c->min_reach;
    if (!c->reached) { c->note[0] = 0; c->hold_s = 0; return; }
    if (fabs(c->angle) > CENTER_DEG) { say(c, "Now back to center."); c->hold_s = 0; return; }
    c->note[0] = 0;
    c->hold_s += dt;
    if (c->hold_s < CENTER_SECONDS) return;

    /* the result: 90 % of the smaller side, in whole degrees, within the axis's range
     * (settings.c motion_full_range); pitch also keeps level + full inside PITCH_LIMIT_DEG,
     * which LEVEL_PITCH_MAX leaves room for, so `hi` never drops below `lo` */
    double reach = fmin(c->hi, -c->lo) * REACH_SHARE;
    int axis = c->step == CALIB_ROLL ? MOTION_ROLL : c->step == CALIB_PITCH ? MOTION_PITCH : MOTION_YAW;
    double lo, hi;
    motion_full_range(axis, &lo, &hi);
    if (axis == MOTION_PITCH) hi = fmax(lo, fmin(hi, PITCH_LIMIT_DEG - fabs(c->level_pitch)));
    c->full[axis] = fmin(hi, fmax(lo, round(reach)));
    enter(c, c->step + 1);                                     /* the next step, or DONE     */
}

/* ------------------------------------------------------------------- update */

/*
 * Feed one sample, after motion_update() has fused it. `saturated`: a raw gyro or
 * accelerometer value reached MOTION_RAW_SATURATED (the reading is clipped).
 */
void calib_update(calib_t *c, motion_t *m, const motion_sample_t *s, bool saturated) {
    if (c->step == CALIB_IDLE || c->step == CALIB_DONE) return;   /* not running          */
    if (!m->seeded) { c->last_t = 0; return; }                 /* no data yet (reconnect)    */
    double dt = m->t - c->last_t;                              /* fusion time since last     */
    c->last_t = m->t;
    if (!(dt > 0) || dt > 0.1) return;                         /* first sample, or a gap     */
    switch (c->step) {
    case CALIB_FLAT:  step_flat(c, m, s, dt); break;
    case CALIB_LEVEL: step_level(c, m, s, dt); break;
    default:          step_reach(c, m, s, dt, saturated); break;
    }
}
