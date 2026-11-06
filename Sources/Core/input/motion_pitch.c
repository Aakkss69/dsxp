/*
 * Motion control, pitch: raise or lower the controller's top edge (the triggers, the
 * nose) to pitch the aircraft. Pure: the fusion (motion.c) passes its "up" vector and the
 * level, and gets back the pitch and the -1..1 for X-Plane's pitch.
 *
 * Pitch from gravity: raising the top edge leans "up" toward -z (the measured frame,
 * motion.h). pitch = atan2(-z, sqrt(x^2 + y^2)) is the elevation of the long axis, the
 * standard pitch-from-gravity formula: exact whatever the bank, independent of heading,
 * and a twist about the vertical leaves it alone. It runs -90..90, so no wrapping.
 *
 * Depends on: shape() (decode.c) for the deadzone and clamp the sticks use too.
 */
#include "core_internal.h"   /* shape()                                                  */
#include "motion_pitch.h"
#include <math.h>

#define DEG (M_PI / 180.0)   /* degrees -> radians                                      */

/* Pitch of an "up" vector, degrees: nose (top edge) up is positive, -90..90. Also what
 * motion.c's out-of-range test reads (near vertical, bank has no meaning). */
double motion_pitch_deg(const double up[3]) {
    return atan2(-up[2], sqrt(up[0] * up[0] + up[1] * up[1])) / DEG;
}

/* Pitch relative to the level's, degrees. Both lie within -90..90, so a plain difference. */
double motion_pitch_angle(const double up[3], const double level_up[3]) {
    return motion_pitch_deg(up) - motion_pitch_deg(level_up);
}

/* The pitch as X-Plane's pitch, -1..1: the angle over the full-deflection angle, through
 * the deadzone (motion.deadzone, shared with roll), clamped, inverted if asked. */
double motion_pitch_output(double angle_deg, const motion_cfg_t *cfg) {
    double full = cfg->full_deg[MOTION_PITCH];
    if (!(full > 0)) return 0;                        /* misconfigured: fly nothing     */
    double v = shape(angle_deg / full, cfg->deadzone_deg / full, 1.0);   /* NaN -> 0    */
    return cfg->invert[MOTION_PITCH] ? -v : v;
}
