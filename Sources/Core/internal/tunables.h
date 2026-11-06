/*
 * Values a user can tune in the config files (the .conf files in configs/). The defaults are
 * defined in settings/settings.c and reproduce the prototype's behavior.
 * Fractions are 0..1 here and shown as percent in the files.
 */
#ifndef DSXP_TUNABLES_H
#define DSXP_TUNABLES_H

#include <stdbool.h>

/* bridge.conf: connection, timing, telemetry, logging */
typedef struct {
    int    port;                    /* X-Plane's UDP receive port              */
    double tick_hz;                 /* core loop rate                          */
    double repeat_hz;               /* "hold" buttons resend rate              */
    double input_stale_s;           /* no input this long: hold controls neutral */
    double input_lost_s;            /* no input this long: hand X-Plane back   */
    double override_reassert_s;     /* re-send wanted overrides this often     */
    int    override_release_repeats;
    double throttle_keepalive_s;    /* resend an unchanged throttle; 0 = never */
    double gate_poll_s;             /* process / USB scan interval             */
    double save_delay_s;            /* mapper changes are saved after this     */
    double telemetry_hz;
    double telemetry_timeout_s;
    double telemetry_resub_s;
    int    log_retention_days;
    int    log_file_cap_mb;
    double report_window_s;
    double idle_report_s;
    bool   log_verbose;
} bridge_cfg_t;

/* controls.conf: touchpad gestures (the sensitivity itself is tuning_t.gesture) */
typedef struct {
    double tap_time_s;              /* a tap must lift within this time        */
    double pulse_s;                 /* how long a gesture "button" stays down  */
    double swipe_length_at0, swipe_length_at100;  /* shortest swipe (pad fraction) at sensitivity 0 / 1 */
    double tap_wander_at0, tap_wander_at100;/* largest tap movement at sensitivity 0 / 1 */
} gesture_cfg_t;

/* feedback.conf: wind-mode trigger model */
typedef struct {
    double full_force_kt;           /* airspeed of full aerodynamic force      */
    double no_airflow_kt;           /* below this: no resistance at all        */
    double free_play;               /* trigger travel before force starts      */
    double base_force;              /* force at any airflow                    */
    double airspeed_force;          /* extra force at full_force_kt            */
    double crosswind_full_kt;           /* crosswind giving full left/right bias   */
    double crosswind_bias;              /* how far a full crosswind shifts force   */
    double buffet_turbulence;             /* turbulence (0-10) where buffet takes over */
    double buffet_start;            /* trigger travel where buffet starts      */
    double buffet_strength_min, buffet_strength_max;
    double buffet_freq_min_hz, buffet_freq_max_hz;
} wind_cfg_t;

/* feedback.conf: sim-mode rumble model */
typedef struct {
    double stall_floor;             /* both motors on a symmetric stall warning */
    double jolt_start_g, jolt_full_g;
    double crash_s;                 /* crash shake duration; 0 = none          */
} rumble_cfg_t;

/* feedback.conf: sim-mode light bar thresholds */
typedef struct {
    double stall_alarm, stall_caution;   /* stall warning ratio 0..1 */
    double g_caution;                    /* |g| */
    double turbulence_caution;                 /* turbulence 0-10 */
} light_sim_cfg_t;

/* controls.conf: motion control - tilt the controller to fly (input/motion.c).
 * Arrays are indexed by the X-Plane axis: MOTION_ROLL, MOTION_PITCH, MOTION_YAW. */
enum { MOTION_ROLL = 0, MOTION_PITCH, MOTION_YAW, MOTION_AXES };
typedef struct {
    bool   enabled;                 /* motion control on                                      */
    bool   use[MOTION_AXES];        /* per axis: true = motion flies it, false = the sticks   */
    double full_deg[MOTION_AXES];   /* controller angle that gives full deflection, degrees  */
    double deadzone_deg;            /* roll and pitch: angle ignored around level, degrees    */
    double yaw_deadzone_deg;        /* yaw: twist ignored around center, degrees              */
    double yaw_return_s;            /* a held twist is ~95 % back to center after this time  */
    bool   invert[MOTION_AXES];     /* reverse an axis                                        */
} motion_cfg_t;

extern const bridge_cfg_t   BRIDGE_DEFAULTS;
extern const motion_cfg_t   MOTION_DEFAULTS;
extern const gesture_cfg_t  GESTURE_DEFAULTS;
extern const wind_cfg_t     WIND_DEFAULTS;
extern const rumble_cfg_t   RUMBLE_DEFAULTS;
extern const light_sim_cfg_t LIGHT_SIM_DEFAULTS;

#endif /* DSXP_TUNABLES_H */
