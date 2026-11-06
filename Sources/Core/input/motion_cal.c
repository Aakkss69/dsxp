/*
 * Motion sensor calibration: the DualSense's feature report 0x05, and turning raw gyro
 * and accelerometer counts into deg/s and g with it. Pure.
 *
 * Follows Linux drivers/hid/hid-playstation.c at tag v6.12:
 *   ps_get_report()                    length, report ID and Bluetooth CRC checks  :747-781
 *   ps_check_crc32()                   CRC-32 over the seed byte, then the report  :700-708
 *   dualsense_get_calibration_data()   field offsets and the scale formulas        :952-1075
 *   dualsense_parse_report()           how the scales are applied                  :1397-1414
 * One deliberate difference: Linux only replaces an axis whose denominator is zero; DSXP
 * also replaces a scale more than MOTION_CAL_TOLERANCE away from nominal, because a wrong
 * scale here tilts the aircraft instead of a game camera.
 */
#include "core_internal.h"
#include <math.h>
#include <stdlib.h>          /* abs() */

/* Signed little-endian 16-bit field at p. Linux reads every calibration field into a
 * `short` (:954-960), so all of them, the "minus" values included, are signed. */
static int le16s(const unsigned char *p) {
    int v = p[0] | (p[1] << 8);              /* 0..65535, low byte first          */
    return v >= 32768 ? v - 65536 : v;       /* upper half of the range: negative */
}

/* Unsigned little-endian 32-bit field at p (the CRC). */
static uint32_t le32u(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* True when `v` is within MOTION_CAL_TOLERANCE of `nominal` (both positive). */
static bool near_nominal(double v, double nominal) {
    return isfinite(v) && fabs(v - nominal) <= MOTION_CAL_TOLERANCE * nominal;
}

/* The scales Linux falls back to for an unusable axis (:1028-1032, :1065-1069). Used for
 * every axis until the controller's own report has been read. */
void motion_cal_nominal(motion_cal_t *cal) {
    for (int i = 0; i < 3; i++) {                        /* x, y, z                     */
        cal->acc_bias[i] = 0.0;                          /* no offset known              */
        cal->acc_per_g[i] = MOTION_ACC_PER_G_NOMINAL;    /* +-4 g across the 16-bit range */
        cal->gyro_per_dps[i] = MOTION_GYRO_PER_DPS_NOMINAL;  /* +-2048 deg/s             */
        cal->gyro_zero_dps[i] = 0.0;                     /* no resting offset known     */
    }
    cal->factory_axes = 0;                               /* nothing from the controller  */
}

/*
 * Parse feature report 0x05 as read from the controller over Bluetooth: buf[0] is the
 * report ID, buf[37..40] the CRC. On any error `cal` holds the nominal scales, so the
 * caller can always use it. On MCAL_OK an axis whose own data is implausible keeps the
 * nominal scale and is not counted in cal->factory_axes.
 */
mcal_result_t motion_cal_parse(const unsigned char *buf, long len, motion_cal_t *cal) {
    motion_cal_nominal(cal);                                   /* the safe starting point  */
    if (!buf || len < MOTION_CAL_REPORT_LEN) return MCAL_SHORT;   /* Linux: "Invalid byte count" */
    if (buf[0] != MOTION_CAL_REPORT_ID) return MCAL_BAD_ID;       /* Linux: "Invalid reportID"   */

    /* CRC-32 (the standard reflected polynomial, crc32_le in output.c) over the seed
     * byte 0xA3 and then bytes 0..36, inverted at the end; stored little-endian. */
    unsigned char seed = MOTION_CAL_CRC_SEED;
    uint32_t crc = crc32_le(&seed, 1, 0xFFFFFFFFu);            /* the seed first           */
    crc = crc32_le(buf, MOTION_CAL_CRC_AT, crc) ^ 0xFFFFFFFFu; /* then the report, invert  */
    if (crc != le32u(buf + MOTION_CAL_CRC_AT)) return MCAL_BAD_CRC;

    /* Field offsets as Linux reads them (:978-994). The gyro block is ordered pitch,
     * yaw, roll, which is the controller's gyro x, y, z (Linux maps them to ABS_RX,
     * ABS_RY, ABS_RZ in that order, :1002-1016). */
    int gyro_bias[3]  = { le16s(buf + 1),  le16s(buf + 3),  le16s(buf + 5)  };  /* x y z */
    int gyro_plus[3]  = { le16s(buf + 7),  le16s(buf + 11), le16s(buf + 15) };
    int gyro_minus[3] = { le16s(buf + 9),  le16s(buf + 13), le16s(buf + 17) };
    int speed_2x      = le16s(buf + 19) + le16s(buf + 21);    /* speed plus + speed minus */
    int acc_plus[3]   = { le16s(buf + 23), le16s(buf + 27), le16s(buf + 31) };
    int acc_minus[3]  = { le16s(buf + 25), le16s(buf + 29), le16s(buf + 33) };

    for (int i = 0; i < 3; i++) {
        /* Gyro: Linux scales to deg/s as raw * speed_2x / denom (:1003-1006), so the
         * counts per deg/s are denom / speed_2x. The gyro bias only enters the
         * denominator; Linux sets the applied bias to 0 (:1003). */
        int denom = abs(gyro_plus[i] - gyro_bias[i]) + abs(gyro_minus[i] - gyro_bias[i]);
        double per_dps = speed_2x > 0 ? (double)denom / speed_2x : 0.0;  /* 0 = unusable */
        if (denom != 0 && near_nominal(per_dps, MOTION_GYRO_PER_DPS_NOMINAL)) {
            cal->gyro_per_dps[i] = per_dps;                     /* the controller's own   */
            cal->factory_axes++;
        }
        /* The bias field as a resting offset, the way SDL applies it (SDL_hidapi_ps5.c
         * release-3.2.10: (raw - bias) * sensitivity, and SDL distrusts a bias over 1024
         * counts). Only a starting point: DSXP measures the real drift when calibrating. */
        if (abs(gyro_bias[i]) <= 1024) cal->gyro_zero_dps[i] = gyro_bias[i] / cal->gyro_per_dps[i];

        /* Accelerometer: plus and minus are the readings at +1 g and -1 g, so their
         * difference spans 2 g and the midpoint is the offset. Linux computes the offset
         * with integer division (:1041), kept here so both agree to the count. */
        int range_2g = acc_plus[i] - acc_minus[i];
        int bias = acc_plus[i] - range_2g / 2;                  /* C division truncates, as in Linux */
        double per_g = range_2g / 2.0;                          /* counts for 1 g          */
        if (range_2g != 0 && near_nominal(per_g, MOTION_ACC_PER_G_NOMINAL) &&
            fabs((double)bias) <= MOTION_CAL_TOLERANCE * MOTION_ACC_PER_G_NOMINAL) {
            cal->acc_bias[i] = bias;                            /* the controller's own   */
            cal->acc_per_g[i] = per_g;
            cal->factory_axes++;
        }
    }
    return MCAL_OK;
}

/* Raw counts -> deg/s and g, the same arithmetic as dualsense_parse_report (:1397-1414)
 * but in floating point (Linux keeps integers scaled to 1/1024 deg/s and 1/8192 g). */
void motion_cal_apply(const motion_cal_t *cal, const int16_t gyro_raw[3],
                      const int16_t acc_raw[3], uint32_t ts, motion_sample_t *out) {
    for (int i = 0; i < 3; i++) {
        out->gyro_dps[i] = gyro_raw[i] / cal->gyro_per_dps[i];                 /* no offset */
        out->acc_g[i] = (acc_raw[i] - cal->acc_bias[i]) / cal->acc_per_g[i];  /* offset first */
    }
    out->ts = ts;                                                             /* passed through */
}

/* Words for the log. */
const char *motion_cal_result_name(mcal_result_t r) {
    switch (r) {
    case MCAL_OK:      return "ok";
    case MCAL_SHORT:   return "too short";
    case MCAL_BAD_ID:  return "wrong report ID";
    case MCAL_BAD_CRC: return "CRC mismatch";
    }
    return "?";
}
