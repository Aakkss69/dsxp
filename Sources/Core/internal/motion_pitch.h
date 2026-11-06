/*
 * Motion control, pitch (input/motion_pitch.c): how far the controller's top edge (the
 * nose) is raised from the calibrated level, and the -1..1 it sends to X-Plane's pitch.
 * Pure.
 *
 * Needs only the fusion's "up" vector (motion.c) and the user's settings (motion_cfg_t);
 * nothing else in this file depends on roll or yaw.
 */
#ifndef DSXP_MOTION_PITCH_H
#define DSXP_MOTION_PITCH_H

#include "tunables.h"                 /* motion_cfg_t, MOTION_PITCH */

double motion_pitch_deg(const double up[3]);                             /* absolute, deg  */
double motion_pitch_angle(const double up[3], const double level_up[3]); /* from level, deg */
double motion_pitch_output(double angle_deg, const motion_cfg_t *cfg);   /* -1..1          */

#endif /* DSXP_MOTION_PITCH_H */
