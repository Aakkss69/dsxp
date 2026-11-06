/* Default values for everything in the config files. Pure. */
#include "core_internal.h"
#include <math.h>            /* ceil */
#include <string.h>

const bridge_cfg_t BRIDGE_DEFAULTS = {
    .port = XP_PORT,
    .tick_hz = TICK_HZ,
    .repeat_hz = CMD_REPEAT_HZ,
    .input_stale_s = INPUT_STALE_SEC,
    .input_lost_s = INPUT_LOST_SEC,
    .override_reassert_s = OVERRIDE_REASSERT_SEC,
    .override_release_repeats = OVERRIDE_RELEASE_REPEAT,
    .throttle_keepalive_s = THROTTLE_KEEPALIVE_SEC,
    .gate_poll_s = GATE_POLL_SEC,
    .save_delay_s = SAVE_DEBOUNCE_SEC,
    .telemetry_hz = TELEMETRY_HZ,
    .telemetry_timeout_s = TELEMETRY_TIMEOUT_SEC,
    .telemetry_resub_s = TELEMETRY_RESUB_SEC,
    .log_retention_days = 14,
    .log_file_cap_mb = 5,
    .report_window_s = REPORT_WINDOW_SEC,
    .idle_report_s = REPORT_IDLE_LOG_SEC,
    .log_verbose = false,
};

/* the prototype's formulas: swipe 0.30 - 0.22*s, tap wander 0.04 + 0.03*(1 - s) */
const gesture_cfg_t GESTURE_DEFAULTS = {
    .tap_time_s = 0.35, .pulse_s = GESTURE_PULSE_SEC,
    .swipe_length_at0 = 0.30, .swipe_length_at100 = 0.08,
    .tap_wander_at0 = 0.07, .tap_wander_at100 = 0.04,
};

/* The prototype worked in 0-255 trigger bytes: free play 30, force 30 + 200 q,
 * buffet start 20, amplitude 60..240, 18..40 Hz, full crosswind 12 m/s. These
 * are the same values in the files' units, each exactly representable at the
 * precision the file is written with (so a round trip changes nothing). */
const wind_cfg_t WIND_DEFAULTS = {
    .full_force_kt = 160, .no_airflow_kt = 5,
    .free_play = 0.118, .base_force = 0.118, .airspeed_force = 0.784,
    .crosswind_full_kt = 23.3, .crosswind_bias = 0.45,
    .buffet_turbulence = 1.5, .buffet_start = 0.078,
    .buffet_strength_min = 0.235, .buffet_strength_max = 0.941,
    .buffet_freq_min_hz = 18, .buffet_freq_max_hz = 40,
};

const rumble_cfg_t RUMBLE_DEFAULTS = {
    .stall_floor = 0.60, .jolt_start_g = 2.5, .jolt_full_g = 6.0, .crash_s = 3.0,
};

const light_sim_cfg_t LIGHT_SIM_DEFAULTS = {
    .stall_alarm = 0.60, .stall_caution = 0.15, .g_caution = 2.5, .turbulence_caution = 1.5,
};

/* Motion control starts off. When switched on, tilt flies roll and pitch and the yaw
 * stays on the sticks and rudder triggers, because twisting a controller is the least
 * natural of the three. Full deflection at 40 deg bank and 30 deg pitch keeps the wrists
 * inside a comfortable range; 2 deg of deadzone hides hand tremor, and yaw gets 4 deg
 * because its angle is integrated, so any leftover gyro drift collects there. */
const motion_cfg_t MOTION_DEFAULTS = {
    .enabled = false,
    .use = { [MOTION_ROLL] = true, [MOTION_PITCH] = true, [MOTION_YAW] = false },
    .full_deg = { [MOTION_ROLL] = 40, [MOTION_PITCH] = 30, [MOTION_YAW] = 30 },
    .deadzone_deg = 2, .yaw_deadzone_deg = 4,
    .yaw_return_s = 4,
    .invert = { false, false, false },
    .calibrated = false,                       /* the mapper asks for the calibration first */
    .level_bank_deg = 0, .level_pitch_deg = 0, /* until then: lying flat is level           */
    .gyro_bias_dps = { 0, 0, 0 },
};

/* The documented range of an axis's full-deflection angle, degrees (controls.conf, the
 * mapper's sliders and the calibration all use it). `axis`: MOTION_ROLL, PITCH or YAW. */
void motion_full_range(int axis, double *lo, double *hi) {
    *lo = axis == MOTION_ROLL ? MOTION_ROLL_FULL_MIN : axis == MOTION_PITCH ? MOTION_PITCH_FULL_MIN
        : MOTION_YAW_FULL_MIN;
    *hi = axis == MOTION_ROLL ? MOTION_ROLL_FULL_MAX : axis == MOTION_PITCH ? MOTION_PITCH_FULL_MAX
        : MOTION_YAW_FULL_MAX;
}

/*
 * Make `axis`'s full-deflection angle valid: inside its range, and at least 1 deg past the
 * axis's deadzone (roll and pitch: motion.deadzone, yaw: motion.yaw.deadzone). Below that
 * the usable range collapses onto a step, and once the deadzone reaches the full angle
 * shape() ignores the deadzone altogether. Raised to whole degrees, as controls.conf
 * writes it. Every writer of motion.full_deg calls this (config_validate, CMD_MOTION, the
 * end of the calibration), so they cannot disagree. Returns true if it had to change.
 * The deadzones' own ranges (10 and 20 deg) keep the result inside the angle's range.
 */
bool motion_full_fix(motion_cfg_t *m, int axis) {
    double lo, hi;
    motion_full_range(axis, &lo, &hi);
    double v = m->full_deg[axis];
    if (!(v >= lo)) v = lo;                                  /* below the range, or NaN    */
    if (v > hi) v = hi;                                      /* above the range            */
    double dz = axis == MOTION_YAW ? m->yaw_deadzone_deg : m->deadzone_deg;   /* its deadzone */
    if (v < dz + 1.0) v = ceil(dz + 1.0);                    /* 1 deg past it, whole degrees */
    bool changed = v != m->full_deg[axis];
    m->full_deg[axis] = v;
    return changed;
}

void config_defaults(config_t *c) {
    memset(c, 0, sizeof *c);
    c->bridge = BRIDGE_DEFAULTS;
    load_defaults(&c->s);
}
