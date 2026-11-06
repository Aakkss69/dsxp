/*
 * Motion control: turns the DualSense's gyro and accelerometer into the bank, pitch and
 * hold-to-steer yaw that can fly X-Plane's roll, pitch and yaw. Pure: no I/O, no threads,
 * no globals, so the tests drive it with synthetic sensor data.
 *
 *   input/motion_cal.c  feature report 0x05 -> calibration; raw counts -> deg/s and g
 *   input/motion.c      sensor fusion -> angles -> per-axis outputs; the crossfade helper
 *
 * Sources: Linux drivers/hid/hid-playstation.c at tag v6.12 (anchors per item below and
 * in docs/REFERENCES.md); the maths references are named where they are used in motion.c.
 *
 * Controller frame, measured with a live tilt test on 2026-11-06 (controller face up,
 * triggers pointing away from you, like holding a yoke):
 *   x  toward the right grip    rolling it left makes accel x positive
 *   y  out of the face          +1 g lying flat, face up
 *   z  toward you (bottom edge) raising the top edge (triggers) makes accel z negative
 *   gyro x = pitch (top edge up is positive), gyro z = roll (left is positive),
 *   gyro y = twist about the face normal.
 * Those five measured signs fix the directions above, and they form a right-handed frame
 * (x right, y up, z back) shared by gyro and accelerometer. In that frame a twist to the
 * left is positive gyro y; that one is derived, not measured, and is checked by the
 * one-way twist test on hardware (motion.c turns it into "twist right = positive yaw").
 * The controller stands in for the aircraft: the top edge is the nose.
 */
#ifndef DSXP_MOTION_H
#define DSXP_MOTION_H

#include <stdbool.h>
#include <stdint.h>
#include "tunables.h"                 /* motion_cfg_t, MOTION_ROLL / PITCH / YAW */

/* ------------------------------------------------------- calibration (motion_cal.c) */

#define MOTION_CAL_REPORT_ID  0x05    /* DS_FEATURE_REPORT_CALIBRATION (:88)             */
#define MOTION_CAL_REPORT_LEN 41      /* DS_FEATURE_REPORT_CALIBRATION_SIZE (:89)        */
#define MOTION_CAL_CRC_SEED   0xA3    /* PS_FEATURE_CRC32_SEED (:77), Bluetooth only     */
#define MOTION_CAL_CRC_AT     37      /* the last 4 bytes hold the CRC (:771-772)        */
/* Fallback scales when the controller's own data is missing or implausible. Linux uses
 * DS_ACC_RANGE / S16_MAX (4 g at full scale) and DS_GYRO_RANGE / S16_MAX (2048 deg/s at
 * full scale) for an axis whose calibration is unusable (:1028-1032, :1065-1069). */
#define MOTION_ACC_PER_G_NOMINAL    (32767.0 / 4.0)      /* counts per g      */
#define MOTION_GYRO_PER_DPS_NOMINAL (32767.0 / 2048.0)   /* counts per deg/s  */
#define MOTION_CAL_TOLERANCE        0.30   /* a factory scale this far off nominal is rejected */

/*
 * What the sensors can report, and what was measured on a real controller over Bluetooth
 * on this Mac (2026-11-07, read-only probe; Sony publishes no figures beyond "six-axis
 * motion sensing", and no teardown names the IMU part):
 *   gyro    16-bit signed; factory scale 16.37-16.39 counts per deg/s, so +-2000 deg/s
 *           at full scale (Linux declares +-2048, DS_GYRO_RANGE :147); 0.061 deg/s per
 *           count. Fast wrist flicks peaked at 762 deg/s, a hard shake at 1182 deg/s:
 *           never saturated.
 *   accel   16-bit signed; factory scale 8189.5-8197 counts per g, so +-4 g at full
 *           scale (Linux DS_ACC_RANGE :145); 0.12 mg per count. A hard shake clipped
 *           at raw 32765 (4.00 g), so fast shaking does saturate it.
 *   rate    one sample per input report: 64 per second, sensor-clock steps 15.0-15.7 ms,
 *           1 % of steps over 40 ms (dropped reports), longest 52.5 ms.
 *   held    in the hand, held level: gyro spread (1 sd) 0.4-0.8 deg/s per axis.
 * A sample within MOTION_RAW_SATURATED of either end is treated as clipped.
 */
#define MOTION_RAW_SATURATED 32700

/* How a calibration attempt ended. */
typedef enum {
    MCAL_OK = 0,      /* parsed; check factory_axes for axes that fell back to nominal */
    MCAL_SHORT,       /* fewer than MOTION_CAL_REPORT_LEN bytes (or no buffer)       */
    MCAL_BAD_ID,      /* byte 0 is not report ID 0x05                                 */
    MCAL_BAD_CRC,     /* the CRC in bytes 37-40 does not match                        */
} mcal_result_t;

/* Where the calibration in use came from (shown in the mapper). */
typedef enum { MOTION_CAL_NONE = 0, MOTION_CAL_NOMINAL, MOTION_CAL_FACTORY } motion_cal_source_t;

/* Per-axis scales: deg/s = raw / gyro_per_dps; g = (raw - acc_bias) / acc_per_g. */
typedef struct {
    double acc_bias[3];      /* counts subtracted from accel x, y, z before scaling   */
    double acc_per_g[3];     /* counts per g, accel x, y, z                           */
    double gyro_per_dps[3];  /* counts per deg/s, gyro x, y, z                        */
    double gyro_zero_dps[3]; /* the factory's resting gyro offset, deg/s (0 if unread). Linux
                                ignores it; SDL subtracts it. Measured: -0.61 -0.55 +0.55
                                from the report vs -1.09 -0.76 +0.56 held in the hand, so
                                DSXP uses it as the starting bias until calibrated        */
    int    factory_axes;     /* axes (of 6) using the controller's own values; 6 = all */
} motion_cal_t;

/* One calibrated sensor reading. */
typedef struct {
    double   gyro_dps[3];    /* angular rate, deg/s, controller frame                 */
    double   acc_g[3];       /* specific force, g (lying still: the "up" direction)   */
    uint32_t ts;             /* sensor clock, 0.33 us ticks (wraps every ~23.9 min)   */
} motion_sample_t;

void          motion_cal_nominal(motion_cal_t *cal);
mcal_result_t motion_cal_parse(const unsigned char *buf, long len, motion_cal_t *cal);
void          motion_cal_apply(const motion_cal_t *cal, const int16_t gyro_raw[3],
                               const int16_t acc_raw[3], uint32_t ts, motion_sample_t *out);
const char   *motion_cal_result_name(mcal_result_t r);

/* ------------------------------------------------------------ fusion (motion.c) */

#define MOTION_TICKS_PER_SEC 3000000.0  /* sensor clock: 0.33 us ticks (:1416)           */
#define MOTION_BLEND_SEC     0.3        /* crossfade length when an axis changes source  */

/* Fusion state. motion_init() once; motion_reset() when the controller reconnects. */
typedef struct {
    /* the filter */
    bool     seeded;          /* up[] has been set from the accelerometer              */
    bool     have_ts;         /* last_ts holds a real reading                          */
    uint32_t last_ts;         /* previous sample's sensor clock                        */
    double   t;               /* seconds of sensor time fused since the last reset     */
    double   up[3];           /* estimated "up" in the controller frame, unit length   */
    /* level reference */
    double   neutral[3];      /* up[] captured by Recenter: this attitude reads as level */
    /* hold-to-steer yaw */
    double   yaw_deg;         /* integrated twist about the vertical, deg               */
    double   yaw_credit;      /* what the return removed since the last zero crossing,
                                 signed like yaw_deg; spent when twisting back          */
    /* gyro bias, learned while the controller is still */
    double   bias[3];         /* deg/s subtracted from the gyro                         */
    bool     bias_learned;    /* at least one still window has been folded in            */
    double   win_start;       /* sensor time the current still window began              */
    double   win_min[3], win_max[3], win_sum[3];   /* raw gyro extremes and sum in it     */
    int      win_n;           /* samples in the window                                    */
    double   quiet_until;     /* learning paused until this sensor time (own vibration)  */
    /* results, refreshed by every motion_update() */
    double   bank_deg;        /* relative to neutral; right wing down is positive        */
    double   pitch_deg;       /* relative to neutral; nose (top edge) up is positive     */
    bool     out_of_range;    /* face down or near vertical: outputs held neutral        */
    double   out[3];          /* -1..1 per MOTION_ROLL / PITCH / YAW after the settings */
    long     samples;         /* samples fused since the last reset                      */
} motion_t;

/* A crossfade from a value that was being sent to whatever the new source says. */
typedef struct { bool active; double from, t0; } motion_blend_t;

void   motion_init(motion_t *m);
void   motion_reset(motion_t *m);
void   motion_update(motion_t *m, const motion_sample_t *s, const motion_cfg_t *cfg, bool quiet);
void   motion_recenter(motion_t *m);
void   motion_set_level(motion_t *m, double bank_deg, double pitch_deg);
void   motion_attitude(const double up[3], double *bank_deg, double *pitch_deg);
void   motion_blend_start(motion_blend_t *b, double from, double now);
double motion_blend(motion_blend_t *b, double target, double now);

/* True when motion, not the sticks, flies this axis (MOTION_ROLL / PITCH / YAW). Never
 * before the calibration has been completed. */
static inline bool motion_owns(const motion_cfg_t *cfg, int axis) {
    return cfg->enabled && cfg->calibrated && axis >= 0 && axis < MOTION_AXES && cfg->use[axis];
}

/* -------------------------------------------------- calibration (motion_calib.c) */

/* The steps, in order. Each completes on its own once the controller shows it. */
typedef enum {
    CALIB_IDLE = 0,   /* not running                                                  */
    CALIB_FLAT,       /* lying face up, untouched: gyro drift, accelerometer check     */
    CALIB_LEVEL,      /* held the way you fly, steady: the level attitude              */
    CALIB_ROLL,       /* bank left and right as far as comfortable, back to level      */
    CALIB_PITCH,      /* top edge up and down as far as comfortable, back to level     */
    CALIB_YAW,        /* twist left and right as far as comfortable, back to center    */
    CALIB_DONE,       /* results ready (copied into the settings by the core)          */
    CALIB_STEPS
} calib_step_t;

/* The calibration's state and results. Plain data: copied into the mapper's snapshot. */
typedef struct {
    int    step;                 /* calib_step_t                                              */
    double hold_s;               /* how long the step's condition has held so far             */
    double need_s;               /* how long it must hold (timed steps), else 0               */
    double angle;                /* reach steps: the live angle from level, degrees          */
    double lo, hi;               /* reach steps: the furthest each way so far                 */
    double min_reach;            /* reach steps: each side must get at least this far         */
    bool   reached;              /* reach steps: both sides far enough, now return to center  */
    char   note[112];            /* a hint for the user ("Keep your hands off it."), or ""   */
    /* working values (not shown) */
    double last_t;               /* fusion time of the previous sample, for time steps       */
    double sum_g[3], min_g[3], max_g[3], sum_a[3];  /* FLAT: the still streak's gyro, accel */
    double sum_up[3];            /* LEVEL: the steady streak's "up" vectors                  */
    long   n;                    /* samples in the streak                                     */
    double twist;                /* YAW: integrated twist since the step began, degrees      */
    /* results */
    double bias[3];              /* resting gyro drift, deg/s                                 */
    double acc_g;                /* magnitude of gravity as measured lying still, g          */
    double level_bank, level_pitch;   /* the level attitude, degrees (absolute)              */
    double full[MOTION_AXES];    /* full-deflection angles from the reach, degrees           */
} calib_t;

void        calib_start(calib_t *c);
void        calib_cancel(calib_t *c);
void        calib_update(calib_t *c, motion_t *m, const motion_sample_t *s, bool saturated);
const char *calib_step_name(int step);

#endif /* DSXP_MOTION_H */
