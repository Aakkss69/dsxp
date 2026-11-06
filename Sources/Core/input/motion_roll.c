/*
 * Motion control, roll: bank the controller to roll the aircraft. Pure: the fusion
 * (motion.c) passes its "up" vector and the level, and gets back the bank and the -1..1
 * for X-Plane's roll.
 *
 * Bank from gravity: rolling right lowers the right grip, so "up" leans toward -x (the
 * measured frame, motion.h: rolling left makes accel x positive). bank = atan2(-x, y) is
 * the standard roll-from-gravity formula (as in an aircraft attitude reference with the
 * nose along -z and the right wing along +x): exact for any pitch followed by any bank,
 * independent of heading, and a twist about the vertical leaves it alone.
 *
 * Depends on: shape() (decode.c) for the deadzone and clamp the sticks use too.
 */
#include "core_internal.h"   /* shape()                                                  */
#include "motion_roll.h"
#include <math.h>

#define DEG (M_PI / 180.0)   /* degrees -> radians                                      */

/* Bank of an "up" vector, degrees: right wing down is positive, -180..180. */
double motion_roll_bank_deg(const double up[3]) {
    return atan2(-up[0], up[1]) / DEG;
}

/* Bank relative to the level's, degrees, folded into -180..180 (so a bank past upside
 * down wraps around instead of reading 300 deg). Subtracting the level's bank (rather
 * than rotating the vector) keeps a level held tilted toward you from mixing pitch in. */
double motion_roll_angle(const double up[3], const double level_up[3]) {
    double d = motion_roll_bank_deg(up) - motion_roll_bank_deg(level_up);
    return fmod(d + 540.0, 360.0) - 180.0;            /* shift, wrap, shift back        */
}

/* The bank as X-Plane's roll, -1..1: the angle over the full-deflection angle, through
 * the deadzone (motion.deadzone), clamped, inverted if asked. A full angle at or below
 * zero (never valid after config_validate) flies nothing. */
double motion_roll_output(double angle_deg, const motion_cfg_t *cfg) {
    double full = cfg->full_deg[MOTION_ROLL];
    if (!(full > 0)) return 0;                        /* misconfigured: fly nothing     */
    double v = shape(angle_deg / full, cfg->deadzone_deg / full, 1.0);   /* NaN -> 0    */
    return cfg->invert[MOTION_ROLL] ? -v : v;
}
