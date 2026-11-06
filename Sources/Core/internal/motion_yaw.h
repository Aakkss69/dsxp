/*
 * Motion control, yaw (input/motion_yaw.c): the controller's twist about the vertical,
 * kept as a steering angle (hold-to-steer), and the -1..1 it sends to X-Plane's yaw. Pure.
 *
 * Needs the gyro rate, the fusion's "up" vector and the fusion's clock (motion.c), and the
 * user's settings (motion_cfg_t); nothing in this file depends on roll or pitch.
 *
 * How the yaw follows the twist (the constants below; motion_yaw.c has the details):
 *   follow  while the hand moves, the yaw follows the twist one to one
 *   hold    once the twist has moved less than YAW_STEADY_DEG over YAW_STEADY_S, the yaw
 *           keeps that window's average: hand tremor does not move the rudder, and it
 *           stays put for as long as the hand holds still (no return to center: twist
 *           back, or Recenter)
 *   drift   while held, change slower than YAW_CREEP_DPS is the gyro's leftover drift and
 *           is taken out, so the rudder cannot creep off center over a long flight
 *   add up  slower deliberate change adds up: YAW_STEADY_DEG away from where the hold
 *           began, the hold lets go and the yaw eases over to the twist
 *   sudden  YAW_FAST_DEG of change within YAW_FAST_S, YAW_FAST_WINDOWS times in a row and
 *           the same way, lets go at once (faster than about 1 deg/s)
 * Every change of value eases with the time constant YAW_EASE_S, so the rudder never steps.
 */
#ifndef DSXP_MOTION_YAW_H
#define DSXP_MOTION_YAW_H

#include <stdbool.h>
#include "tunables.h"                 /* motion_cfg_t, MOTION_YAW */

#define YAW_STEADY_DEG   1.0   /* deg: the twist moved less than this ...                  */
#define YAW_STEADY_S     0.5   /* s:   ... over this long: hold (also how far a slow
                                  change must add up before the hold lets go)              */
#define YAW_FAST_DEG     0.1   /* deg: a change of at least this ...                       */
#define YAW_FAST_S       0.1   /* s:   ... within this long ...                            */
#define YAW_FAST_WINDOWS 2     /*      ... this many times in a row, the same way: let go  */
#define YAW_CREEP_DPS    0.25  /* deg/s: while held, slower change is drift: taken out
                                  (leftover gyro drift measured 0.01-0.1 deg/s)            */
#define YAW_EASE_S       0.1   /* s: time constant of every eased change of value          */
#define YAW_WINDUP       2.0   /* the twist is capped at this many times its full angle    */
#define YAW_HISTORY      128   /* twist samples kept: YAW_STEADY_S at up to 250 a second
                                  (the controller sends 64 a second over Bluetooth)        */

/* The yaw's state. motion_yaw_reset() starts it at center, following. */
typedef struct {
    double deg;          /* the yaw flown, deg, right positive (what the output uses)       */
    double raw;          /* the twist added up, deg, with the drift taken out while held    */
    bool   holding;      /* true: held (deg eases to `held`); false: following the twist    */
    double held;         /* while holding: the average twist of the steady window, deg      */
    double anchor;       /* while holding: the twist when the hold began (what slow change
                            adds up from, and what the drift is held to), deg               */
    double offset;       /* while following: deg - raw, easing to 0 (no step on letting go) */
    double hist_t[YAW_HISTORY];   /* recent samples: fusion time, s ...                     */
    double hist_v[YAW_HISTORY];   /* ... and the twist then, deg (a ring buffer)           */
    int    hist_head;    /* index of the newest sample                                       */
    int    hist_n;       /* samples held, up to YAW_HISTORY                                 */
} motion_yaw_t;

void   motion_yaw_reset(motion_yaw_t *y);
double motion_yaw_rate(const double gyro_dps[3], const double bias_dps[3], const double up[3]);
void   motion_yaw_update(motion_yaw_t *y, double rate_dps, double dt, double now,
                         const motion_cfg_t *cfg);
double motion_yaw_output(const motion_yaw_t *y, const motion_cfg_t *cfg);   /* -1..1 */

#endif /* DSXP_MOTION_YAW_H */
