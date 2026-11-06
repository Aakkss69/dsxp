/*
 * Motion control, yaw: twist the controller about its face, like a steering wheel lying
 * flat, to yaw the aircraft. Pure: the fusion (motion.c) passes the twist rate and its
 * clock each sample, and gets back the yaw and the -1..1 for X-Plane's yaw.
 *
 * There is no compass to say where "straight" is, so the twist is the gyro's rate about
 * the vertical added up over time, and the yaw is held as a steering angle. The rules
 * (constants in motion_yaw.h, each step in motion_yaw_update):
 *   follow  while the hand moves, the yaw follows the twist one to one
 *   hold    once the twist has moved less than YAW_STEADY_DEG over the last YAW_STEADY_S,
 *           the yaw eases to that window's average and stays there: hand tremor does not
 *           wiggle the rudder, and nothing pulls it back to center while the hand is still
 *   drift   while held, the twist is pulled back to where the hold began at up to
 *           YAW_CREEP_DPS: the gyro's leftover drift (0.01-0.1 deg/s measured) is slower
 *           than that and is taken out, so it never adds up into rudder
 *   add up  a slower deliberate twist outruns the pull: once it is YAW_STEADY_DEG from
 *           where the hold began, the hold lets go and the yaw eases over to it
 *   sudden  a change of YAW_FAST_DEG within YAW_FAST_S, in YAW_FAST_WINDOWS windows in a
 *           row and the same way, lets go at once; a bump that goes and comes back does
 *           not (its two windows point opposite ways)
 * Letting go keeps the yaw where it was and follows the hand from there, the difference
 * easing out over YAW_EASE_S, so neither holding nor letting go ever steps the rudder.
 *
 * Depends on: shape() (decode.c) for the deadzone and clamp the sticks use too.
 */
#include "core_internal.h"   /* motion_yaw.h (through motion.h), shape()                 */
#include <math.h>
#include <string.h>

/* Back to center, following, with no history. */
void motion_yaw_reset(motion_yaw_t *y) {
    memset(y, 0, sizeof *y);                          /* deg = raw = 0, not holding    */
}

/*
 * The twist rate about the true vertical, deg/s, twist right positive: the gyro's rate
 * (bias removed) along "up". Twisting right (clockwise seen from above) is a negative
 * rotation about +up, hence the minus. Used by the fusion and by the calibration's
 * twist step (motion_calib.c), so both measure the same twist.
 */
double motion_yaw_rate(const double gyro_dps[3], const double bias_dps[3], const double up[3]) {
    double r = 0;
    for (int i = 0; i < 3; i++) r -= (gyro_dps[i] - bias_dps[i]) * up[i];
    return r;
}

/* ------------------------------------------------------------- history */

/* Remember the twist `v` at fusion time `t` (the oldest sample drops out when full). */
static void remember(motion_yaw_t *y, double t, double v) {
    y->hist_head = (y->hist_head + 1) % YAW_HISTORY;  /* the next slot                  */
    y->hist_t[y->hist_head] = t;
    y->hist_v[y->hist_head] = v;
    if (y->hist_n < YAW_HISTORY) y->hist_n++;
}

/* Slot of the i-th newest sample (0 = newest). */
static int slot(const motion_yaw_t *y, int i) {
    return (y->hist_head - i + YAW_HISTORY) % YAW_HISTORY;
}

/* The twist at time `at`: the newest sample taken at or before it. False when the
 * history does not reach back that far. */
static bool twist_at(const motion_yaw_t *y, double at, double *v) {
    for (int i = 0; i < y->hist_n; i++) {
        int k = slot(y, i);
        if (y->hist_t[k] <= at) { *v = y->hist_v[k]; return true; }
    }
    return false;
}

/* Range (max - min) and average of the twist over the last `span` seconds. False when
 * the history does not cover the whole span yet. */
static bool window(const motion_yaw_t *y, double now, double span, double *range, double *mean) {
    if (y->hist_n == 0 || y->hist_t[slot(y, y->hist_n - 1)] > now - span) return false;
    double lo = 0, hi = 0, sum = 0;
    int n = 0;
    for (int i = 0; i < y->hist_n; i++) {
        int k = slot(y, i);
        if (y->hist_t[k] < now - span) break;         /* older than the window: done   */
        double v = y->hist_v[k];
        if (n == 0 || v < lo) lo = v;                 /* widen the range               */
        if (n == 0 || v > hi) hi = v;
        sum += v;
        n++;
    }
    *range = hi - lo;
    *mean = sum / n;                                  /* n >= 1: the newest is inside  */
    return true;
}

/* A sudden movement: in each of the last YAW_FAST_WINDOWS windows of YAW_FAST_S the twist
 * changed by at least YAW_FAST_DEG, every one the same way. */
static bool sudden(const motion_yaw_t *y, double now) {
    double v[YAW_FAST_WINDOWS + 1];                   /* the twist at each window edge */
    for (int i = 0; i <= YAW_FAST_WINDOWS; i++)
        if (!twist_at(y, now - i * YAW_FAST_S, &v[i])) return false;   /* too little history */
    int way = 0;                                      /* +1 or -1 once known           */
    for (int i = 0; i < YAW_FAST_WINDOWS; i++) {
        double d = v[i] - v[i + 1];                   /* window i: newer minus older   */
        if (fabs(d) < YAW_FAST_DEG) return false;     /* too small in this window      */
        int w = d > 0 ? 1 : -1;
        if (way && w != way) return false;            /* went one way, then the other  */
        way = w;
    }
    return true;
}

/* ------------------------------------------------------------- update */

/*
 * One sample: `rate_dps` from motion_yaw_rate(), `dt` seconds since the last sample, `now`
 * the fusion's clock (motion_t.t). The fusion calls this only while the controller is in
 * range; when it is not, it resets the yaw instead.
 */
void motion_yaw_update(motion_yaw_t *y, double rate_dps, double dt, double now,
                       const motion_cfg_t *cfg) {
    y->raw += rate_dps * dt;                          /* the twist, added up           */
    if (y->holding) {                                 /* drift: pulled back, slowly    */
        double creep = YAW_CREEP_DPS * dt;            /* the most taken out per sample */
        y->raw -= fmax(-creep, fmin(creep, y->raw - y->anchor));
    }
    double cap = YAW_WINDUP * cfg->full_deg[MOTION_YAW];   /* no endless wind-up       */
    y->raw = fmax(-cap, fmin(cap, y->raw));
    remember(y, now, y->raw);

    /* hold, or let go */
    bool fast = sudden(y, now);
    if (y->holding) {
        if (fast || fabs(y->raw - y->anchor) >= YAW_STEADY_DEG) {   /* sudden, or added up */
            y->holding = false;
            y->offset = y->deg - y->raw;              /* carry on from where it is     */
        }
    } else {
        double range, mean;
        if (!fast && window(y, now, YAW_STEADY_S, &range, &mean) && range < YAW_STEADY_DEG) {
            y->holding = true;                        /* steady: hold the average      */
            y->held = mean;
            y->anchor = y->raw;
        }
    }

    /* the yaw flown: eased to the held value, or the twist plus a fading difference */
    double k = fmin(1.0, dt / YAW_EASE_S);            /* share of the gap closed now   */
    if (y->holding) {
        y->deg += (y->held - y->deg) * k;
    } else {
        y->offset -= y->offset * k;
        y->deg = y->raw + y->offset;
    }
}

/* The yaw as X-Plane's yaw, -1..1: the angle over the full-deflection angle, through the
 * yaw's own deadzone (motion.yaw.deadzone), clamped, inverted if asked. */
double motion_yaw_output(const motion_yaw_t *y, const motion_cfg_t *cfg) {
    double full = cfg->full_deg[MOTION_YAW];
    if (!(full > 0)) return 0;                        /* misconfigured: fly nothing    */
    double v = shape(y->deg / full, cfg->yaw_deadzone_deg / full, 1.0);   /* NaN -> 0  */
    return cfg->invert[MOTION_YAW] ? -v : v;
}
