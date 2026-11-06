/*
 * Motion control, roll (input/motion_roll.c): the controller's bank, relative to the
 * calibrated level, and the -1..1 it sends to X-Plane's roll. Pure.
 *
 * Needs only the fusion's "up" vector (motion.c) and the user's settings (motion_cfg_t);
 * nothing else in this file depends on pitch or yaw.
 */
#ifndef DSXP_MOTION_ROLL_H
#define DSXP_MOTION_ROLL_H

#include "tunables.h"                 /* motion_cfg_t, MOTION_ROLL */

double motion_roll_bank_deg(const double up[3]);                        /* absolute bank, deg */
double motion_roll_angle(const double up[3], const double level_up[3]); /* from level, deg    */
double motion_roll_output(double angle_deg, const motion_cfg_t *cfg);   /* -1..1              */

#endif /* DSXP_MOTION_ROLL_H */
