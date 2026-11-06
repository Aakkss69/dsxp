/*
 * Motion control sensor fusion: calibrated gyro + accelerometer -> bank, pitch and
 * hold-to-steer yaw -> one -1..1 output per X-Plane axis. Pure: the caller passes every
 * sample and every setting in, and time comes only from the controller's own clock.
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
 *   3. Bank and pitch relative to the "level" the user captured with Recenter.
 *   4. Yaw: the twist rate about the vertical, integrated while held, easing back to
 *      center on its own, and never overshooting past center on the way back.
 *   5. Gyro bias learned whenever the controller is still.
 *   6. Each axis through the user's sensitivity (full-deflection angle), deadzone and
 *      invert, using the same shape() as the sticks.
 */
#include "core_internal.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ tuning */

#define DEG       (M_PI / 180.0)   /* degrees -> radians                                    */
#define TAU_S     0.5     /* accelerometer correction time constant: lower trusts the
                             accelerometer more (steadier level, more wobble when moved)    */
#define ACC_TRUST 0.10    /* skip the correction when |a| is this far from 1 g (shaking)    */
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
#define YAW_WINDUP    2.0 /* the held twist is capped at this many times its full angle     */

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

/*
 * Bank and pitch of an "up" vector, in the aircraft convention: pitch is the elevation of
 * the controller's long axis (the nose), bank the rotation about that axis. These are the
 * standard roll/pitch-from-gravity formulas (as in an aircraft attitude reference with the
 * nose along -z and the right wing along +x): exact for any pitch followed by any bank,
 * independent of heading.
 *   Bank: rolling right lowers the right grip, so "up" leans toward -x (the measured frame:
 *         rolling left makes x positive).
 *   Pitch: raising the top edge leans "up" toward -z (toward the nose).
 */
static void attitude_deg(const double u[3], double *bank, double *pitch) {
    *bank  = atan2(-u[0], u[1]) / DEG;
    *pitch = atan2(-u[2], sqrt(u[0] * u[0] + u[1] * u[1])) / DEG;
}

/* a - b for angles, folded into -180..180 (a bank past upside down wraps around). */
static double angle_diff(double a, double b) {
    double d = fmod(a - b + 540.0, 360.0) - 180.0;   /* shift, wrap, shift back          */
    return d;
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
    m->yaw_deg = 0;
    m->yaw_credit = 0;
    m->win_start = 0;
    m->win_n = 0;
    m->quiet_until = 0;
    m->bank_deg = m->pitch_deg = 0;
    m->out_of_range = false;
    m->out[0] = m->out[1] = m->out[2] = 0;       /* nothing to fly with yet         */
    m->samples = 0;
}

/* The calibrated level: the "up" vector of a controller at this bank and pitch (the
 * inverse of attitude_deg: bank = atan2(-x, y), pitch = atan2(-z, hypot(x, y))). */
void motion_set_level(motion_t *m, double bank_deg, double pitch_deg) {
    double b = bank_deg * DEG, p = pitch_deg * DEG;
    m->neutral[0] = -sin(b) * cos(p);            /* leans toward -x when banked right  */
    m->neutral[1] =  cos(b) * cos(p);            /* through the face                    */
    m->neutral[2] = -sin(p);                     /* toward -z when the nose is up       */
    m->yaw_deg = 0;                              /* a new level starts with no twist    */
    m->yaw_credit = 0;
}

/* Bank and pitch of an "up" vector, degrees (for the calibration and the mapper). */
void motion_attitude(const double up[3], double *bank_deg, double *pitch_deg) {
    attitude_deg(up, bank_deg, pitch_deg);
}

/* Recenter: the attitude held now becomes level, and the held twist is dropped. */
void motion_recenter(motion_t *m) {
    if (m->seeded) memcpy(m->neutral, m->up, sizeof m->neutral);   /* no data: keep the old level */
    m->yaw_deg = 0;
    m->yaw_credit = 0;
}

/* ------------------------------------------------------------- the steps */

/* Step 5: gyro bias. A window of still samples (every axis inside BIAS_RANGE) whose mean
 * is near the current bias is folded in; anything else just restarts the window. Paused
 * while DSXP itself vibrates the controller (`quiet` false), which shakes the gyro. */
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

/* Step 4: hold-to-steer yaw. `rate` is deg/s about the vertical, positive = twist right. */
static void integrate_yaw(motion_t *m, double rate, double dt, const motion_cfg_t *cfg) {
    double d = rate * dt;                                /* twist during this sample      */
    /* Twisting back toward center while the return has already brought the angle part of
     * the way: that part of the twist is absorbed, so putting the controller back where
     * it started ends at center instead of overshooting into opposite rudder. */
    if (d * m->yaw_credit < 0) {                         /* opposite signs: heading back  */
        double a = fmin(fabs(d), fabs(m->yaw_credit));   /* absorb at most the credit     */
        d += d > 0 ? -a : a;                             /* smaller return twist          */
        m->yaw_credit += m->yaw_credit > 0 ? -a : a;     /* credit spent                  */
    }
    double before = m->yaw_deg;
    m->yaw_deg += d;
    if (before * m->yaw_deg < 0) m->yaw_credit = 0;      /* crossed center: credit void   */
    double cap = YAW_WINDUP * cfg->full_deg[MOTION_YAW]; /* no endless wind-up           */
    if (m->yaw_deg > cap) m->yaw_deg = cap;
    if (m->yaw_deg < -cap) m->yaw_deg = -cap;
    /* The return: exponential decay toward center, with a time constant of a third of
     * yaw_return_s, so the twist is about 95 % back (e^-3) after yaw_return_s. What it
     * removes is remembered as credit (same sign as the angle). */
    double tau = cfg->yaw_return_s / 3.0;
    double ret = tau > 0 ? fmin(1.0, dt / tau) : 1.0;
    double leak = m->yaw_deg * ret;
    m->yaw_deg -= leak;
    m->yaw_credit += leak;
}

/* Step 6: angle -> -1..1 for one axis, with the user's settings. */
static double axis_output(double angle, double full, double deadzone, bool invert) {
    if (!(full > 0)) return 0;                           /* misconfigured: fly nothing    */
    double v = shape(angle / full, deadzone / full, 1.0);   /* deadzone, clamp, NaN -> 0  */
    return invert ? -v : v;
}

/* Bank, pitch, out-of-range and the three outputs, from the current state. */
static void refresh_outputs(motion_t *m, const motion_cfg_t *cfg) {
    /* Step 3: relative to the captured level, by subtracting its bank and pitch. For the
     * usual grip (held tilted toward you, then rolled about the controller's long axis)
     * this is exact; rotating the vector instead would mix some roll into pitch. */
    double bank, pitch, bank0, pitch0;
    attitude_deg(m->up, &bank, &pitch);                  /* the controller now          */
    attitude_deg(m->neutral, &bank0, &pitch0);           /* the captured level           */
    m->bank_deg  = angle_diff(bank, bank0);
    m->pitch_deg = pitch - pitch0;                       /* both within -90..90          */
    /* Out of range is judged on the controller itself, not relative to level: face
     * turned down or sideways (little "up" through the face), or the long axis near
     * vertical, where bank has no meaning. */
    m->out_of_range = m->up[1] < OOR_UP || fabs(pitch) > OOR_PITCH_DEG;
    if (m->out_of_range) {                               /* face down, or stood on edge   */
        m->out[0] = m->out[1] = m->out[2] = 0;           /* hold every axis neutral        */
        return;
    }
    m->out[MOTION_ROLL]  = axis_output(m->bank_deg,  cfg->full_deg[MOTION_ROLL],
                                       cfg->deadzone_deg, cfg->invert[MOTION_ROLL]);
    m->out[MOTION_PITCH] = axis_output(m->pitch_deg, cfg->full_deg[MOTION_PITCH],
                                       cfg->deadzone_deg, cfg->invert[MOTION_PITCH]);
    m->out[MOTION_YAW]   = axis_output(m->yaw_deg,   cfg->full_deg[MOTION_YAW],
                                       cfg->yaw_deadzone_deg, cfg->invert[MOTION_YAW]);
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
        if (!normalize3(a)) return;                      /* no direction (free fall, junk) */
        memcpy(m->up, a, sizeof m->up);                  /* seed: trust the accelerometer   */
        m->seeded = true;
        m->yaw_deg = m->yaw_credit = 0;                  /* a held twist is lost in a gap   */
        m->win_n = 0;
        refresh_outputs(m, cfg);
        return;
    }
    if (dt <= 0) return;                                 /* same timestamp: a duplicate     */
    m->t += dt;
    m->samples++;

    learn_bias(m, s, quiet);                             /* step 5 (raw rates)              */

    /* Step 2a: rotate the estimate with the gyro. A direction fixed in the world, seen
     * from the turning controller, turns the opposite way: du/dt = -(w x u). Checked
     * against the measured frame: rolling left (gyro z > 0) gives du/dt = +x. */
    double w_dps[3], w[3], wxu[3];
    for (int i = 0; i < 3; i++) {
        w_dps[i] = s->gyro_dps[i] - m->bias[i];          /* bias removed                    */
        w[i] = w_dps[i] * DEG;                           /* rad/s                            */
    }
    cross3(w, m->up, wxu);
    for (int i = 0; i < 3; i++) m->up[i] -= wxu[i] * dt;

    /* Step 2b: pull toward the accelerometer, unless the controller is being shaken
     * (then the accelerometer is not showing gravity alone). */
    if (fabs(a_len - 1.0) <= ACC_TRUST && normalize3(a)) {
        double k = fmin(1.0, dt / TAU_S);                /* share of the gap closed now     */
        for (int i = 0; i < 3; i++) m->up[i] += k * (a[i] - m->up[i]);
    }
    if (!normalize3(m->up)) {                            /* cannot happen in practice        */
        m->seeded = false;                               /* re-seed from the next sample     */
        return;
    }

    /* Step 4: the twist rate about the true vertical is the gyro's component along "up";
     * twisting right (clockwise seen from above) is a negative rotation about +up, hence
     * the minus. Not integrated while out of range (lying on a table, face down). */
    refresh_outputs(m, cfg);                             /* updates out_of_range first      */
    if (!m->out_of_range) integrate_yaw(m, -dot3(w_dps, m->up), dt, cfg);
    else m->yaw_deg = m->yaw_credit = 0;                 /* set down: nothing held          */
    m->out[MOTION_YAW] = m->out_of_range ? 0 :
        axis_output(m->yaw_deg, cfg->full_deg[MOTION_YAW], cfg->yaw_deadzone_deg,
                    cfg->invert[MOTION_YAW]);            /* yaw output after this sample   */
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
