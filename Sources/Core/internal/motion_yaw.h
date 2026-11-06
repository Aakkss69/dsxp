/*
 * Motion control, yaw (input/motion_yaw.c): the controller's twist about its own face,
 * kept as a steering angle (hold-to-steer), and the -1..1 it sends to X-Plane's yaw. Pure.
 *
 * Needs the gyro and its bias, and the fusion's clock (motion.c), and the user's settings
 * (motion_cfg_t); nothing in this file depends on roll or pitch. The twist is the gyro's
 * own y axis (the face normal), so banking and pitching the controller, which turn it
 * about its other two axes, never read as twist.
 *
 * How the yaw follows the twist (the constants below; motion_yaw.c has the details):
 *   follow  while the hand moves, the yaw follows the twist one to one
 *   hold    once the twist has moved less than YAW_STEADY_DEG over YAW_STEADY_S, the yaw
 *           keeps that window's average: hand tremor does not move the rudder, and it
 *           stays put for as long as the hand holds it (twist back, or Recenter, to center)
 *   add up  slower change adds up; once it is YAW_STEADY_DEG away from where the hold
 *           began, the pace over the last YAW_DRIFT_S decides:
 *             slower than YAW_CREEP_DPS: the gyro's drift; taken out, and YAW_DRIFT_SHARE
 *               of that pace is subtracted from every sample from then on
 *             faster: a deliberate twist; the hold lets go and the whole twist counts
 *   sudden  YAW_FAST_DEG of change within YAW_FAST_S, YAW_FAST_WINDOWS times in a row and
 *           the same way, lets go at once (faster than about 1 deg/s)
 *   set down  lying as still as on a table (motion.c), the yaw eases back to center
 *   gaps    a reporting gap up to YAW_GAP_KEEP_S keeps the yaw; a longer one centers it
 * Every change of value eases with the time constant YAW_EASE_S, so the rudder never steps.
 */
#ifndef DSXP_MOTION_YAW_H
#define DSXP_MOTION_YAW_H

#include <stdbool.h>
#include "tunables.h"                 /* motion_cfg_t, MOTION_YAW */

#define YAW_STEADY_DEG   1.0   /* deg: the twist moved less than this ...                  */
#define YAW_STEADY_S     0.5   /* s:   ... over this long: hold (also how far a slow
                                  change must add up before it is judged)                  */
#define YAW_FAST_DEG     0.1   /* deg: a change of at least this ...                       */
#define YAW_FAST_S       0.1   /* s:   ... within this long ...                            */
#define YAW_FAST_WINDOWS 2     /*      ... this many times in a row, the same way: let go  */
#define YAW_CREEP_DPS    0.25  /* deg/s: added up slower than this, the change is drift
                                  (leftover gyro drift measured 0.01-0.1 deg/s)            */
#define YAW_DRIFT_S      2.0   /* s: how far back that pace is measured                    */
#define YAW_DRIFT_SHARE  0.5   /* share of each measured drift subtracted from then on     */
#define YAW_EASE_S       0.1   /* s: time constant of every eased change of value          */
#define YAW_GAP_KEEP_S   1.0   /* s: a reporting gap up to this long keeps the yaw (the
                                  hand barely moves in it); a longer one centers it        */
#define YAW_WINDUP       2.0   /* the twist is capped at this many times its full angle    */
#define YAW_HISTORY      256   /* twist samples kept: YAW_DRIFT_S at up to 127 a second
                                  (the controller sends 64 a second over Bluetooth)        */

/* The yaw's state. motion_yaw_reset() starts it at center, following. */
typedef struct {
    double deg;          /* the yaw flown, deg, right positive (what the output uses)       */
    double raw;          /* the twist added up, deg, with measured drift taken out          */
    bool   holding;      /* true: held (deg eases to `held`); false: following the twist    */
    double held;         /* while holding: the average twist of the steady window, deg      */
    double anchor;       /* while holding: the twist when the hold began, deg               */
    double offset;       /* while following: deg - raw, easing to 0 (no step on letting go) */
    double drift_dps;    /* measured leftover drift, deg/s, subtracted from every sample    */
    double drift_bias;   /* the gyro y bias that drift was measured against, deg/s          */
    double hist_t[YAW_HISTORY];   /* recent samples: fusion time, s ...                     */
    double hist_v[YAW_HISTORY];   /* ... and the twist then, deg (a ring buffer)           */
    int    hist_head;    /* index of the newest sample                                       */
    int    hist_n;       /* samples held, up to YAW_HISTORY                                 */
} motion_yaw_t;

void   motion_yaw_reset(motion_yaw_t *y);
void   motion_yaw_gap(motion_yaw_t *y, double gap_s);
void   motion_yaw_rest(motion_yaw_t *y);
double motion_yaw_rate(const double gyro_dps[3], const double bias_dps[3]);
void   motion_yaw_update(motion_yaw_t *y, const double gyro_dps[3], const double bias_dps[3],
                         double dt, double now, const motion_cfg_t *cfg);
double motion_yaw_output(const motion_yaw_t *y, const motion_cfg_t *cfg);   /* -1..1 */

#endif /* DSXP_MOTION_YAW_H */
