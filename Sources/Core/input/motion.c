/*
 * Motion control sensor fusion: calibrated gyro + accelerometer -> the controller's "up"
 * and twist rate, handed to one file per movement for the -1..1 each X-Plane axis gets:
 *   motion_roll.c   bank  -> roll       motion_pitch.c  pitch -> pitch
 *   motion_yaw.c    twist -> yaw (hold-to-steer)
 * This file keeps what all three share: time, the "up" filter, the gyro bias, the level,
 * and whether the controller can be read at all. Pure: the caller passes every sample and
 * every setting in, and time comes only from the controller's own clock.
 *
 * The method, step by step (motion_update):
 *   1. Time step from the sensor clock, so Bluetooth jitter and dropped reports do not
 *      distort the integration.
 *   2. "Up" vector filter. The gyro turns the estimate as the controller turns; the
 *      accelerometer, which points up when nothing else accelerates the controller,
 *      pulls it back a little every sample. This is a complementary filter on the
 *      gravity direction (the same idea as Mahony's filter without the integral term:
 *      R. Mahony, T. Hamel, J.-M. Pflimlin, "Nonlinear Complementary Filters on the
 *      Special Orthogonal Group", IEEE TAC 53(5), 2008), reduced to the one vector this
 *      feature needs. Bank and pitch come from it exactly, whatever the heading.
 *   3. Bank (motion_roll.c) and pitch (motion_pitch.c) relative to the level: the
 *      calibrated one (motion_set_level), or the attitude captured by Recenter; and
 *      whether the controller is out of range (face down, or stood on its edge).
 *   4. Yaw (motion_yaw.c): the twist about the controller's face (so bank and pitch
 *      never read as twist), kept as a steering angle that holds while the hand is still.
 *   5. Gyro bias learned whenever the controller is still; and whether it has been set
 *      down (lying as still as on a table), which centers the yaw.
 *   6. Each axis through the user's sensitivity (full-deflection angle), deadzone and
 *      invert, in its own file, using the same shape() as the sticks.
 */
#include "core_internal.h"
#include "motion_pitch.h"
#include "motion_roll.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ tuning */

#define DEG       (M_PI / 180.0)   /* degrees -> radians                                    */
#define TAU_S     0.5     /* accelerometer correction time constant: lower trusts the
                             accelerometer more (steadier level, more wobble when moved)    */
#define ACC_TRUST 0.10    /* skip the correction when |a| is this far from 1 g (shaking)    */
/* Seeding "up" from one sample: only from one within SEED_TRUST of 1 g (a jolt or free
 * fall reads far from it; a jolted seed read 39 deg of bank on a level controller), then
 * SETTLE_S of a faster correction (time constant SETTLE_TAU_S), so a seed that a smaller
 * jolt still bent fades in a tenth of a second instead of half a second. */
#define SEED_TRUST    0.25
#define SETTLE_S      0.5
#define SETTLE_TAU_S  0.1
#define DT_MAX_S  0.1     /* a longer gap is a break: re-seed from the accelerometer       */
#define BIAS_WIN_S    1.0 /* still-window length for bias learning                          */
#define BIAS_RANGE    1.5 /* deg/s: every gyro axis must stay within this range in a window */
#define BIAS_NEAR     1.5 /* deg/s: the window mean must be this close to the current bias,
                             so a slow deliberate turn (2 deg/s and up) is never mistaken
                             for drift; the measured bias was under 1 deg/s per axis        */
#define BIAS_RATE     0.2 /* share of the difference folded in per window (~5 s average)   */
#define QUIET_HOLD_S  0.5 /* learning stays paused this long after DSXP's own vibration     */
#define OOR_UP        0.2 /* "up" this close to horizontal (or below) is out of range       */
#define OOR_PITCH_DEG 70  /* pitch beyond this is out of range (controller stood on edge)   */
/* Set down: on a table every gyro axis stayed within 2.5 deg/s peak-to-peak over each 3 s
 * stretch (137 of 137 stretches), while the stillest 3 s held in a hand spanned 4.0 deg/s
 * (measured 2026-11-07, motion_calib.c FLAT). So 3 s within 2.5 means nobody holds it. */
#define REST_P2P_DPS  2.5
#define REST_S        3.0

/* ------------------------------------------------------------- vector helpers */

static double dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* out = a x b (right-handed cross product). out may not alias a or b. */
static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Scale v to unit length; returns false (v unchanged) if it is too short to have a
 * direction. */
static bool normalize3(double v[3]) {
    double n = sqrt(dot3(v, v));                 /* length                       */
    if (!(n > 1e-9)) return false;               /* also catches NaN             */
    v[0] /= n; v[1] /= n; v[2] /= n;
    return true;
}

/* ----------------------------------------------------------------- lifecycle */

/* Everything back to start: level is "lying flat, face up", no bias known. */
void motion_init(motion_t *m) {
    memset(m, 0, sizeof *m);
    m->neutral[1] = 1.0;                         /* flat, face up, reads as level  */
    motion_reset(m);
}

/* The controller reconnected or the data stream broke: restart the filter and the
 * held yaw. The captured level and the learned bias stay, they belong to the user's
 * grip and to this controller. */
void motion_reset(motion_t *m) {
    m->seeded = false;                           /* next sample seeds up[]          */
    m->have_ts = false;                          /* next sample has no time step    */
    m->t = 0;
    m->settle_until = 0;
    m->awaiting_seed = false;
    motion_yaw_reset(&m->yaw);                   /* a held twist ends with the stream */
    m->win_start = 0;
    m->win_n = 0;
    m->quiet_until = 0;
    m->rest_n = 0;
    m->resting = false;
    m->bank_deg = m->pitch_deg = 0;
    m->out_of_range = false;
    m->out[0] = m->out[1] = m->out[2] = 0;       /* nothing to fly with yet         */
    m->samples = 0;
}

/* The calibrated level: the "up" vector of a controller at this bank and pitch (the
 * inverse of motion_roll_bank_deg, bank = atan2(-x, y), and motion_pitch_deg,
 * pitch = atan2(-z, hypot(x, y))). */
void motion_set_level(motion_t *m, double bank_deg, double pitch_deg) {
    double b = bank_deg * DEG, p = pitch_deg * DEG;
    m->neutral[0] = -sin(b) * cos(p);            /* leans toward -x when banked right  */
    m->neutral[1] =  cos(b) * cos(p);            /* through the face                    */
    m->neutral[2] = -sin(p);                     /* toward -z when the nose is up       */
    motion_yaw_reset(&m->yaw);                   /* a new level starts with no twist    */
}

/* Bank and pitch of an "up" vector, degrees (for the calibration and the mapper). */
void motion_attitude(const double up[3], double *bank_deg, double *pitch_deg) {
    *bank_deg = motion_roll_bank_deg(up);
    *pitch_deg = motion_pitch_deg(up);
}

/* Recenter: the attitude held now becomes level, and the held twist is dropped. Refused
 * (nothing changes, returns false) when that attitude cannot be a level: no data yet, out
 * of range (face down or on its edge: held level afterwards, bank would read about 180
 * and flip between full left and full right), or banked or pitched past
 * MOTION_LEVEL_MAX_DEG, the same limit the calibration's LEVEL step applies. */
bool motion_recenter(motion_t *m) {
    if (!m->seeded || m->out_of_range) return false;     /* nothing usable to level on     */
    double bank, pitch;
    motion_attitude(m->up, &bank, &pitch);
    if (fabs(bank) > MOTION_LEVEL_MAX_DEG || fabs(pitch) > MOTION_LEVEL_MAX_DEG) return false;
    memcpy(m->neutral, m->up, sizeof m->neutral);        /* this attitude reads as level   */
    motion_yaw_reset(&m->yaw);
    return true;
}

/* ------------------------------------------------------------- the steps */

/* Step 5: gyro bias. A window of still samples (every axis inside BIAS_RANGE) whose mean
 * is near the current bias is folded in; anything else just restarts the window. Paused
 * while DSXP itself vibrates the controller (`quiet` true), which shakes the gyro, and for
 * QUIET_HOLD_S after. */
static void learn_bias(motion_t *m, const motion_sample_t *s, bool quiet) {
    if (quiet) m->quiet_until = m->t + QUIET_HOLD_S;     /* our motors or a vibrating trigger */
    if (m->t < m->quiet_until) { m->win_n = 0; return; } /* not yet: drop the window      */
    if (m->win_n == 0) {                                 /* first sample of a window      */
        m->win_start = m->t;
        for (int i = 0; i < 3; i++) {
            m->win_min[i] = m->win_max[i] = s->gyro_dps[i];
            m->win_sum[i] = 0;
        }
    }
    bool still = true;
    for (int i = 0; i < 3; i++) {
        double g = s->gyro_dps[i];
        if (g < m->win_min[i]) m->win_min[i] = g;        /* widen the window's range     */
        if (g > m->win_max[i]) m->win_max[i] = g;
        m->win_sum[i] += g;
        if (m->win_max[i] - m->win_min[i] > BIAS_RANGE) still = false;
    }
    m->win_n++;
    if (!still) { m->win_n = 0; return; }                /* moved: start over             */
    if (m->t - m->win_start < BIAS_WIN_S) return;        /* window not full yet           */
    double mean[3];
    bool near = true;
    for (int i = 0; i < 3; i++) {
        mean[i] = m->win_sum[i] / m->win_n;
        if (fabs(mean[i] - m->bias[i]) > BIAS_NEAR) near = false;   /* a real slow turn */
    }
    if (near) {
        for (int i = 0; i < 3; i++) m->bias[i] += BIAS_RATE * (mean[i] - m->bias[i]);
        m->bias_learned = true;
    }
    m->win_n = 0;                                        /* next window                   */
}

/* Step 5b: set down. A stretch of REST_S with every gyro axis within REST_P2P_DPS is
 * stiller than any hand: nobody is steering, so the yaw eases to center (once per
 * stretch; any movement starts a new one). Raw rates: the bias does not matter here. */
static void watch_rest(motion_t *m, const motion_sample_t *s) {
    if (m->rest_n == 0) {                                /* a new stretch starts here     */
        m->rest_start = m->t;
        for (int i = 0; i < 3; i++) m->rest_min[i] = m->rest_max[i] = s->gyro_dps[i];
    }
    bool still = true;
    for (int i = 0; i < 3; i++) {
        double g = s->gyro_dps[i];
        if (g < m->rest_min[i]) m->rest_min[i] = g;      /* widen the stretch's range     */
        if (g > m->rest_max[i]) m->rest_max[i] = g;
        if (m->rest_max[i] - m->rest_min[i] > REST_P2P_DPS) still = false;
    }
    m->rest_n++;
    if (!still) { m->rest_n = 0; m->resting = false; return; }   /* picked up, or held  */
    if (!m->resting && m->t - m->rest_start >= REST_S) {
        m->resting = true;
        motion_yaw_rest(&m->yaw);                        /* motion_yaw.c: ease to center  */
    }
}

/* Step 3: bank, pitch and out-of-range from the current "up". */
static void refresh_attitude(motion_t *m) {
    m->bank_deg  = motion_roll_angle(m->up, m->neutral);    /* motion_roll.c          */
    m->pitch_deg = motion_pitch_angle(m->up, m->neutral);   /* motion_pitch.c         */
    /* Out of range is judged on the controller itself, not relative to level: face
     * turned down or sideways (little "up" through the face), or the long axis near
     * vertical, where bank has no meaning. */
    m->out_of_range = m->up[1] < OOR_UP || fabs(motion_pitch_deg(m->up)) > OOR_PITCH_DEG;
}

/* Step 6: each axis's -1..1, from its own file; all neutral while out of range. */
static void refresh_outputs(motion_t *m, const motion_cfg_t *cfg) {
    if (m->out_of_range) {                               /* face down, or stood on edge   */
        m->out[0] = m->out[1] = m->out[2] = 0;           /* hold every axis neutral        */
        return;
    }
    m->out[MOTION_ROLL]  = motion_roll_output(m->bank_deg, cfg);
    m->out[MOTION_PITCH] = motion_pitch_output(m->pitch_deg, cfg);
    m->out[MOTION_YAW]   = motion_yaw_output(&m->yaw, cfg);
}

/* ------------------------------------------------------------- the update */

/*
 * Fuse one calibrated sample. `quiet` is true while DSXP is vibrating the controller
 * (rumble motors or a vibrate trigger effect); it only pauses bias learning.
 */
void motion_update(motion_t *m, const motion_sample_t *s, const motion_cfg_t *cfg, bool quiet) {
    /* Step 1: the time step. The clock is unsigned 32-bit, so the subtraction wraps
     * correctly across its ~23.9-minute rollover (Linux handles the wrap the same way,
     * :1418-1423). */
    double dt = m->have_ts ? (uint32_t)(s->ts - m->last_ts) / MOTION_TICKS_PER_SEC : 0;
    bool fresh = m->have_ts;                             /* there was a sample before      */
    m->last_ts = s->ts;
    m->have_ts = true;

    double a[3] = { s->acc_g[0], s->acc_g[1], s->acc_g[2] };
    double a_len = sqrt(dot3(a, a));                     /* 1 g when only gravity acts     */

    if (!m->seeded || !fresh || dt > DT_MAX_S) {         /* first sample, or a break        */
        if (m->seeded && fresh) motion_yaw_gap(&m->yaw, dt);   /* a break: kept if short   */
        else if (!m->awaiting_seed) motion_yaw_reset(&m->yaw);  /* the first: from center */
        /* only a sample showing gravity alone can seed "up"; until one comes the outputs
         * stay as they were (a few samples at most when the controller is held) */
        if (!(fabs(a_len - 1.0) <= SEED_TRUST) || !normalize3(a)) {
            m->seeded = false;
            m->awaiting_seed = true;                     /* keep the yaw for that sample   */
            return;
        }
        m->awaiting_seed = false;
        memcpy(m->up, a, sizeof m->up);                  /* seed: trust the accelerometer   */
        m->seeded = true;
        m->settle_until = m->t + SETTLE_S;               /* then settle quickly            */
        m->win_n = 0;
        m->rest_n = 0;
        refresh_attitude(m);
        refresh_outputs(m, cfg);
        return;
    }
    if (dt <= 0) return;                                 /* same timestamp: a duplicate     */
    m->t += dt;
    m->samples++;

    learn_bias(m, s, quiet);                             /* step 5 (raw rates)              */
    watch_rest(m, s);                                    /* step 5b: set down?              */

    /* Step 2a: rotate the estimate with the gyro. A direction fixed in the world, seen
     * from the turning controller, turns the opposite way: du/dt = -(w x u). Checked
     * against the measured frame: rolling left (gyro z > 0) gives du/dt = +x. */
    double w[3], wxu[3];
    for (int i = 0; i < 3; i++) w[i] = (s->gyro_dps[i] - m->bias[i]) * DEG;   /* bias removed, rad/s */
    cross3(w, m->up, wxu);
    for (int i = 0; i < 3; i++) m->up[i] -= wxu[i] * dt;

    /* Step 2b: pull toward the accelerometer, unless the controller is being shaken
     * (then the accelerometer is not showing gravity alone). */
    if (fabs(a_len - 1.0) <= ACC_TRUST && normalize3(a)) {
        double tau = m->t < m->settle_until ? SETTLE_TAU_S : TAU_S;   /* just seeded: fast */
        double k = fmin(1.0, dt / tau);                  /* share of the gap closed now     */
        for (int i = 0; i < 3; i++) m->up[i] += k * (a[i] - m->up[i]);
    }
    if (!normalize3(m->up)) {                            /* cannot happen in practice        */
        m->seeded = false;                               /* re-seed from the next sample     */
        return;
    }

    refresh_attitude(m);                                 /* step 3, and out_of_range        */
    /* Step 4 (motion_yaw.c): the twist about the controller's face, added up and held.
     * Not while out of range (face down, or stood on its edge): nothing is held then. */
    if (!m->out_of_range)
        motion_yaw_update(&m->yaw, s->gyro_dps, m->bias, dt, m->t, cfg);
    else
        motion_yaw_reset(&m->yaw);
    refresh_outputs(m, cfg);                             /* step 6, once, after the yaw     */
}

/* ------------------------------------------------------------- crossfade */

/* Begin a crossfade from `from` (the value X-Plane was last sent) at time `now`. */
void motion_blend_start(motion_blend_t *b, double from, double now) {
    b->active = true;
    b->from = from;
    b->t0 = now;
}

/* The value to send now: eases from b->from to `target` over MOTION_BLEND_SEC with a
 * smoothstep (zero slope at both ends, so the control surface starts and stops gently). */
double motion_blend(motion_blend_t *b, double target, double now) {
    if (!b->active) return target;                       /* no fade running                 */
    double x = (now - b->t0) / MOTION_BLEND_SEC;         /* 0 at the start, 1 at the end    */
    if (!(x < 1.0) || x < 0.0) {                         /* done (or the clock went back)   */
        b->active = false;
        return target;
    }
    double e = x * x * (3.0 - 2.0 * x);                  /* smoothstep                      */
    return b->from + (target - b->from) * e;
}
