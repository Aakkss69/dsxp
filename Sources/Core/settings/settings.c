/* Default values for everything in the config files. Pure. */
#include "core_internal.h"
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
    .swipe_at0 = 0.30, .swipe_at100 = 0.08,
    .wander_at0 = 0.07, .wander_at100 = 0.04,
};

/* The prototype worked in 0-255 trigger bytes: free play 30, force 30 + 200 q,
 * buffet start 20, amplitude 60..240, 18..40 Hz, full crosswind 12 m/s. These
 * are the same values in the files' units, each exactly representable at the
 * precision the file is written with (so a round trip changes nothing). */
const wind_cfg_t WIND_DEFAULTS = {
    .full_force_kt = 160, .no_airflow_kt = 5,
    .free_play = 0.118, .base_force = 0.118, .airspeed_force = 0.784,
    .xwind_full_kt = 23.3, .xwind_bias = 0.45,
    .buffet_turb = 1.5, .buffet_start = 0.078,
    .buffet_str_min = 0.235, .buffet_str_max = 0.941,
    .buffet_hz_min = 18, .buffet_hz_max = 40,
};

const rumble_cfg_t RUMBLE_DEFAULTS = {
    .stall_floor = 0.60, .jolt_start_g = 2.5, .jolt_full_g = 6.0, .crash_s = 3.0,
};

const lightsim_cfg_t LIGHTSIM_DEFAULTS = {
    .stall_alarm = 0.60, .stall_caution = 0.15, .g_caution = 2.5, .turb_caution = 1.5,
};

void config_defaults(config_t *c) {
    memset(c, 0, sizeof *c);
    c->bridge = BRIDGE_DEFAULTS;
    load_defaults(&c->s);
}
