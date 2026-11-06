/*
 * Motion control, yaw: twist the controller about its face, like a steering wheel lying
 * flat, to yaw the aircraft. Pure: the fusion (motion.c) passes the gyro, its bias and its
 * clock each sample, and gets back the yaw and the -1..1 for X-Plane's yaw.
 *
 * The twist is the turn about the controller's own face normal (gyro y), the wrist's own
 * twist. Banking turns the controller about its long axis (gyro z) and pitching about its
 * side axis (gyro x), so neither reads as twist, whatever the level, and tilting it round
 * in circles adds nothing up. (The world vertical would not do: with the level held 15 deg
 * nose up, a 30 deg bank turns the controller 7.8 deg about the vertical, and every small
 * circle of the tilt turns it a little more.)
 *
 * There is no compass to say where "straight" is, so the twist is added up over time and
 * held as a steering angle. The rules (constants in motion_yaw.h, steps in
 * motion_yaw_update):
 *   follow  while the hand moves, the yaw follows the twist one to one
 *   hold    once the twist has moved less than YAW_STEADY_DEG over the last YAW_STEADY_S,
 *           the yaw eases to that window's average and stays there: hand tremor does not
 *           wiggle the rudder, and nothing pulls it back while the hand holds it
 *   add up  while held, slower change adds up. Once it is YAW_STEADY_DEG from where the
 *           hold began, its pace over the last YAW_DRIFT_S decides what it was:
 *             under YAW_CREEP_DPS  the gyro's leftover drift (0.01-0.1 deg/s measured):
 *                                  taken out, and part of it is subtracted from every
 *                                  sample from then on, so drift does not add up while
 *                                  the hand moves either
 *             faster               a deliberate slow twist: the hold lets go and the
 *                                  whole twist counts
 *   sudden  a change of YAW_FAST_DEG within YAW_FAST_S, in YAW_FAST_WINDOWS windows in a
 *           row and the same way, lets go at once; a bump that goes and comes back does
 *           not (its two windows point opposite ways)
 * Letting go keeps the yaw where it was and follows the hand from there, the difference
 * easing out over YAW_EASE_S, so neither holding nor letting go ever steps the rudder.
 * motion.c also calls motion_yaw_rest() when the controller has been set down, and
 * motion_yaw_gap() after a gap in the reports.
 *
 * Depends on: shape() (decode.c) for the deadzone and clamp the sticks use too.
 */
#include "core_internal.h"   /* motion_yaw.h (through motion.h), shape()                 */
#include <math.h>
#include <string.h>

/* Back to center, following, with no history. The measured drift is the gyro's, not the
 * twist's, so it stays (motion_yaw_update starts it afresh when the bias changes). */
void motion_yaw_reset(motion_yaw_t *y) {
    double drift = y->drift_dps, against = y->drift_bias;
    memset(y, 0, sizeof *y);                          /* deg = raw = 0, not holding    */
    y->drift_dps = drift;
    y->drift_bias = against;
}

/* The reports stopped for `gap_s` seconds. The fusion's clock does not count the gap, so
 * after a short one the yaw carries on as if nothing happened (the hand barely moved);
 * after a long one, or an unknown one, it starts again from center. */
void motion_yaw_gap(motion_yaw_t *y, double gap_s) {
    if (!(gap_s <= YAW_GAP_KEEP_S)) motion_yaw_reset(y);   /* long, or NaN            */
}

/* The controller has been set down (motion.c: lying as still as on a table). Nobody is
 * steering, so the way it lies becomes center and the yaw eases there; picking it up
 * starts from center rather than from whatever heading it was put down at. */
void motion_yaw_rest(motion_yaw_t *y) {
    y->raw = y->anchor = y->held = 0;                 /* the way it lies is center     */
    y->holding = true;                                /* deg eases to held (0)         */
    y->hist_n = 0;                                    /* the twist so far is done with */
}

/*
 * The twist rate, deg/s, twist right positive: the turn about the controller's face
 * normal, bias removed. The measured frame (motion.h) makes a twist to the left positive
 * gyro y, hence the minus. Used by the fusion and by the calibration's twist step
 * (motion_calib.c), so both measure the same twist.
 */
double motion_yaw_rate(const double gyro_dps[3], const double bias_dps[3]) {
    return -(gyro_dps[1] - bias_dps[1]);
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

/* Move the twist, and everything remembered of it, by `d` deg (taking drift out keeps
 * the history in step, so the windows below never see a jump). */
static void shift(motion_yaw_t *y, double d) {
    y->raw += d;
    for (int i = 0; i < y->hist_n; i++) y->hist_v[slot(y, i)] += d;
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

/* The twist's pace, deg/s, from the sample at or before `now - span` (or the oldest one,
 * when the history is shorter) to now. False with fewer than two samples to go on. */
static bool pace_over(const motion_yaw_t *y, double now, double span, double *pace) {
    if (y->hist_n < 2) return false;
    int k = slot(y, y->hist_n - 1);                   /* the oldest, unless ...        */
    for (int i = 1; i < y->hist_n; i++)               /* ... one is old enough         */
        if (y->hist_t[slot(y, i)] <= now - span) { k = slot(y, i); break; }
    double elapsed = now - y->hist_t[k];
    if (!(elapsed > 0)) return false;
    *pace = (y->raw - y->hist_v[k]) / elapsed;
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

/* Stop holding: carry on from where the yaw is, following the twist from here. */
static void let_go(motion_yaw_t *y) {
    y->holding = false;
    y->offset = y->deg - y->raw;                      /* eases out: no step            */
}

/* ------------------------------------------------------------- update */

/*
 * One sample: the calibrated gyro and its bias (deg/s), `dt` seconds since the last
 * sample, `now` the fusion's clock (motion_t.t). The fusion calls this only while the
 * controller is in range; when it is not, it resets the yaw instead.
 */
void motion_yaw_update(motion_yaw_t *y, const double gyro_dps[3], const double bias_dps[3],
                       double dt, double now, const motion_cfg_t *cfg) {
    if (bias_dps[1] != y->drift_bias) {               /* the bias was re-learned: the  */
        y->drift_dps = 0;                             /*   drift is measured afresh    */
        y->drift_bias = bias_dps[1];
    }
    double rate = motion_yaw_rate(gyro_dps, bias_dps) - y->drift_dps;   /* deg/s        */
    if (!isfinite(rate) || !(dt > 0)) return;         /* nothing usable: as it was     */
    y->raw += rate * dt;                              /* the twist, added up           */
    double cap = YAW_WINDUP * cfg->full_deg[MOTION_YAW];   /* no endless wind-up       */
    y->raw = fmax(-cap, fmin(cap, y->raw));
    remember(y, now, y->raw);

    /* hold, or let go */
    bool fast = sudden(y, now);
    if (y->holding) {
        double moved = y->raw - y->anchor;            /* added up since the hold began */
        if (fast) {
            let_go(y);                                /* sudden: follow at once        */
        } else if (fabs(moved) >= YAW_STEADY_DEG) {
            double pace;
            if (pace_over(y, now, YAW_DRIFT_S, &pace) && fabs(pace) < YAW_CREEP_DPS) {
                shift(y, -moved);                     /* drift: taken out ...          */
                double d = y->drift_dps + YAW_DRIFT_SHARE * pace;   /* ... and from now on */
                y->drift_dps = fmax(-YAW_CREEP_DPS, fmin(YAW_CREEP_DPS, d));
            } else {
                let_go(y);                            /* a slow twist: it all counts   */
            }
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
