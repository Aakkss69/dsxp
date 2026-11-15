/*
 * Motion control: calibration report 0x05 (motion_cal.c) and sensor fusion (motion.c).
 * The fusion tests drive a simulated controller: an exact orientation is turned by a
 * commanded body rate at 64 Hz (the DualSense's Bluetooth rate), and the "sensors" read
 * that rate (plus an optional bias) and the gravity direction in the controller frame.
 */
#include "tests.h"
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------- report 0x05 fixture */

/* A calibration as the fields Linux names. The accelerometer values and the gyro speed
 * are the ones a real controller returned on 2026-11-06; the gyro bias / plus / minus are
 * illustrative values chosen to give that controller's measured denominators
 * (17693, 17703, 17680 counts for 1080 deg/s). */
typedef struct {
    int gyro_bias[3], gyro_plus[3], gyro_minus[3];   /* x (pitch), y (yaw), z (roll) */
    int speed_plus, speed_minus;
    int acc_plus[3], acc_minus[3];
} cal_fields_t;

static const cal_fields_t REAL = {
    .gyro_bias  = { -2, 3, 1 },
    .gyro_plus  = { 8844, 8854, 8841 },          /* bias + 8846, + 8851, + 8840          */
    .gyro_minus = { -8849, -8849, -8839 },       /* bias - 8847, - 8852, - 8840          */
    .speed_plus = 540, .speed_minus = 540,
    .acc_plus   = { 8183, 8143, 8193 },
    .acc_minus  = { -8207, -8251, -8186 },
};

static void put16(unsigned char *p, int v) {             /* signed little-endian 16-bit */
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* Lay the fields out at Linux's offsets and seal the report with its Bluetooth CRC. */
static void make_cal_report(unsigned char *b, const cal_fields_t *f) {
    memset(b, 0, MOTION_CAL_REPORT_LEN);
    b[0] = MOTION_CAL_REPORT_ID;
    for (int i = 0; i < 3; i++) put16(b + 1 + 2 * i, f->gyro_bias[i]);   /* bytes 1-6 */
    put16(b + 7,  f->gyro_plus[0]);  put16(b + 9,  f->gyro_minus[0]);    /* pitch     */
    put16(b + 11, f->gyro_plus[1]);  put16(b + 13, f->gyro_minus[1]);    /* yaw       */
    put16(b + 15, f->gyro_plus[2]);  put16(b + 17, f->gyro_minus[2]);    /* roll      */
    put16(b + 19, f->speed_plus);    put16(b + 21, f->speed_minus);
    for (int i = 0; i < 3; i++) {
        put16(b + 23 + 4 * i, f->acc_plus[i]);                            /* x, y, z +  */
        put16(b + 25 + 4 * i, f->acc_minus[i]);                           /* x, y, z -  */
    }
    unsigned char seed = MOTION_CAL_CRC_SEED;
    uint32_t crc = crc32_le(&seed, 1, 0xFFFFFFFFu);
    crc = crc32_le(b, MOTION_CAL_CRC_AT, crc) ^ 0xFFFFFFFFu;
    for (int k = 0; k < 4; k++) b[MOTION_CAL_CRC_AT + k] = (unsigned char)(crc >> (8 * k));
}

static void nominal_axis(const motion_cal_t *c, int i, bool gyro) {
    if (gyro) { CHECK_NEAR(c->gyro_per_dps[i], MOTION_GYRO_PER_DPS_NOMINAL, 1e-12); }
    else {
        CHECK_NEAR(c->acc_per_g[i], MOTION_ACC_PER_G_NOMINAL, 1e-12);
        CHECK_NEAR(c->acc_bias[i], 0.0, 1e-12);
    }
}

void test_motion_cal(void) {
    unsigned char b[64];
    motion_cal_t c;

    /* the CRC routine is the standard CRC-32: check value of "123456789" */
    const unsigned char nine[] = "123456789";
    CHECK((crc32_le(nine, 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu) == 0xCBF43926u);

    /* a good report: every axis from the controller, with Linux's formulas */
    make_cal_report(b, &REAL);
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN, &c), MCAL_OK);
    CHECK_INT(c.factory_axes, 6);
    for (int i = 0; i < 3; i++) {
        int den = abs(REAL.gyro_plus[i] - REAL.gyro_bias[i]) + abs(REAL.gyro_minus[i] - REAL.gyro_bias[i]);
        CHECK_NEAR(c.gyro_per_dps[i], den / 1080.0, 1e-12);          /* speed_2x = 1080 */
    }
    CHECK_NEAR(c.acc_bias[0], -12, 1e-12);       /* 8183 - 16390/2                       */
    CHECK_NEAR(c.acc_per_g[0], 8195, 1e-12);
    CHECK_NEAR(c.acc_bias[1], -54, 1e-12);       /* 8143 - 16394/2                       */
    CHECK_NEAR(c.acc_bias[2], 4, 1e-12);         /* 8193 - 16379/2, integer division      */
    CHECK_NEAR(c.acc_per_g[2], 8189.5, 1e-12);
    CHECK_INT(motion_cal_parse(b, 64, &c), MCAL_OK);   /* a longer buffer is fine        */

    /* applying it: one g on y, 100 deg/s on x */
    motion_sample_t s;
    int16_t g[3] = { (int16_t)lround(100 * c.gyro_per_dps[0]), 0, 0 };
    int16_t a[3] = { (int16_t)c.acc_bias[0], (int16_t)(c.acc_bias[1] + c.acc_per_g[1]), (int16_t)c.acc_bias[2] };
    motion_cal_apply(&c, g, a, 1234, &s);
    CHECK_NEAR(s.gyro_dps[0], 100.0, 0.1);
    CHECK_NEAR(s.acc_g[0], 0.0, 1e-9);
    CHECK_NEAR(s.acc_g[1], 1.0, 1e-9);
    CHECK(s.ts == 1234);

    /* corrupted, wrong ID, short, missing: refused, and the nominal scales are left */
    make_cal_report(b, &REAL);
    b[24] ^= 0x01;
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN, &c), MCAL_BAD_CRC);
    for (int i = 0; i < 3; i++) { nominal_axis(&c, i, true); nominal_axis(&c, i, false); }
    CHECK_INT(c.factory_axes, 0);
    make_cal_report(b, &REAL);
    b[0] = 0x09;
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN, &c), MCAL_BAD_ID);
    make_cal_report(b, &REAL);
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN - 1, &c), MCAL_SHORT);
    CHECK_INT(motion_cal_parse(NULL, 64, &c), MCAL_SHORT);

    /* a zero denominator (Linux's own check) and an implausible scale (DSXP's extra
     * check) fall back to nominal on that axis only */
    cal_fields_t bad = REAL;
    bad.gyro_plus[1] = bad.gyro_minus[1] = bad.gyro_bias[1];     /* yaw: denominator 0     */
    bad.acc_plus[2] = 400; bad.acc_minus[2] = -400;              /* z: 400 counts per g    */
    make_cal_report(b, &bad);
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN, &c), MCAL_OK);
    CHECK_INT(c.factory_axes, 4);
    nominal_axis(&c, 1, true);
    nominal_axis(&c, 2, false);
    CHECK(c.gyro_per_dps[0] != MOTION_GYRO_PER_DPS_NOMINAL);     /* the others kept        */
    bad = REAL;
    bad.speed_plus = bad.speed_minus = 0;                        /* no speed: every gyro axis */
    make_cal_report(b, &bad);
    CHECK_INT(motion_cal_parse(b, MOTION_CAL_REPORT_LEN, &c), MCAL_OK);
    CHECK_INT(c.factory_axes, 3);
    for (int i = 0; i < 3; i++) nominal_axis(&c, i, true);
    CHECK(strcmp(motion_cal_result_name(MCAL_BAD_CRC), "CRC mismatch") == 0);
}

/* --------------------------------------------------------- simulated controller */

#define SIM_HZ 64.0

typedef struct {
    double   R[3][3];     /* controller -> world; columns are the controller's axes */
    double   bias[3];     /* added to the gyro reading, deg/s                       */
    uint32_t ts;          /* sensor clock                                           */
} sim_t;

static void sim_init(sim_t *s, uint32_t ts) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 3; i++) s->R[i][i] = 1;  /* flat, face up: frames coincide    */
    s->ts = ts;
}

/* Turn by the body rate w (deg/s) for dt: R = R * rot(w dt) (Rodrigues). */
static void sim_turn(sim_t *s, const double w[3], double dt) {
    double ang = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt * M_PI / 180.0;
    if (ang < 1e-15) return;
    double n = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    double k[3] = { w[0] / n, w[1] / n, w[2] / n }, c = cos(ang), sn = sin(ang), r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double kk = k[i] * k[j] * (1 - c);
            double kx = (i == j) ? c : 0;
            if (i == 0 && j == 1) kx = -k[2] * sn;
            if (i == 0 && j == 2) kx =  k[1] * sn;
            if (i == 1 && j == 0) kx =  k[2] * sn;
            if (i == 1 && j == 2) kx = -k[0] * sn;
            if (i == 2 && j == 0) kx = -k[1] * sn;
            if (i == 2 && j == 1) kx =  k[0] * sn;
            r[i][j] = kk + kx;
        }
    double o[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            o[i][j] = 0;
            for (int q = 0; q < 3; q++) o[i][j] += s->R[i][q] * r[q][j];
        }
    memcpy(s->R, o, sizeof o);
}

/* One 64 Hz step at body rate w: turn, then read the sensors. */
static void sim_step(sim_t *s, const double w[3], motion_sample_t *out) {
    double dt = 1.0 / SIM_HZ;
    sim_turn(s, w, dt);
    s->ts += (uint32_t)(dt * MOTION_TICKS_PER_SEC);
    for (int i = 0; i < 3; i++) {
        out->gyro_dps[i] = w[i] + s->bias[i];
        out->acc_g[i] = s->R[1][i];              /* world up (0,1,0) in the controller frame = row 1 */
    }
    out->ts = s->ts;
}

/* Run for `secs` at body rate w; returns the last sample. */
static motion_sample_t run(motion_t *m, sim_t *s, const motion_cfg_t *cfg, double wx, double wy,
                           double wz, double secs) {
    motion_sample_t smp = { { 0 }, { 0 }, 0 };
    double w[3] = { wx, wy, wz };
    int n = (int)lround(secs * SIM_HZ);
    for (int i = 0; i < n; i++) {
        sim_step(s, w, &smp);
        motion_update(m, &smp, cfg, false);
    }
    return smp;
}

/* Held in a hand, otherwise still: `secs` of tremor, +-2 deg/s about the side axis in
 * turn (no net turn). 4 deg/s peak to peak, like the stillest hand measured, so the fusion
 * never takes it for set down (motion.c REST_P2P_DPS 2.5). */
static void hold_hand(motion_t *m, sim_t *s, const motion_cfg_t *cfg, double secs) {
    for (int i = 0; i < (int)lround(secs * SIM_HZ); i++)
        run(m, s, cfg, (i % 2) ? 2 : -2, 0, 0, 1.0 / SIM_HZ);
}

/* Start a fusion at rest, flat, with one sample to seed it. */
static void start(motion_t *m, sim_t *s, const motion_cfg_t *cfg) {
    motion_init(m);
    sim_init(s, 1000);
    run(m, s, cfg, 0, 0, 0, 0.1);
}

/* One 64 Hz step with an exact attitude change and an optional gyro offset: run the
 * fusion, then the calibration (as core.c motion_feed does). */
static void calib_run(calib_t *k, motion_t *m, sim_t *s, const motion_cfg_t *cfg,
                      double wx, double wy, double wz, double secs, bool sat) {
    double w[3] = { wx, wy, wz };
    motion_sample_t smp;
    int n = (int)lround(secs * SIM_HZ);
    for (int i = 0; i < n; i++) {
        sim_step(s, w, &smp);
        motion_update(m, &smp, cfg, false);
        calib_update(k, m, &smp, sat);
    }
}

/* A calibration that has just finished FLAT on a still, flat controller without drift. */
static void calib_to_level(calib_t *k, motion_t *m, sim_t *s, const motion_cfg_t *cfg) {
    motion_init(m);
    sim_init(s, 7000);
    calib_start(k);
    calib_run(k, m, s, cfg, 0, 0, 0, 3.3, false);
}

/* `secs` of a still controller, as calib_run, but with `quiet` passed to the fusion (true:
 * DSXP is vibrating it) and the accelerometer scaled by `acc_scale`. */
static void calib_run_raw(calib_t *k, motion_t *m, sim_t *s, const motion_cfg_t *cfg,
                          double secs, bool quiet, double acc_scale) {
    double z[3] = { 0, 0, 0 };
    motion_sample_t smp;
    for (int i = 0; i < (int)lround(secs * SIM_HZ); i++) {
        sim_step(s, z, &smp);
        for (int j = 0; j < 3; j++) smp.acc_g[j] *= acc_scale;
        motion_update(m, &smp, cfg, quiet);
        calib_update(k, m, &smp, false);
    }
}

/* The calibration routine, step by step, on a simulated controller with a real drift. */
void test_motion_calib(void) {
    static motion_t m;
    static calib_t k;
    sim_t s;
    motion_cfg_t cfg = MOTION_DEFAULTS;
    motion_init(&m);
    sim_init(&s, 5000);
    s.bias[0] = -0.68; s.bias[1] = -0.80; s.bias[2] = 0.54;      /* measured at rest      */
    calib_start(&k);
    CHECK_INT(k.step, CALIB_FLAT);
    CHECK(strcmp(calib_step_name(k.step), "flat") == 0);

    /* FLAT: moving resets the streak; 3 s untouched completes it */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.5, false);
    CHECK(k.hold_s > 1.3 && k.step == CALIB_FLAT);
    calib_run(&k, &m, &s, &cfg, 20, 0, 0, 0.1, false);           /* nudged               */
    CHECK_NEAR(k.hold_s, 0, 1e-12);
    CHECK(k.note[0] != 0);
    calib_run(&k, &m, &s, &cfg, -20, 0, 0, 0.1, false);          /* back flat            */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 3.2, false);
    CHECK_INT(k.step, CALIB_LEVEL);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(k.bias[i], s.bias[i], 1e-6);                  /* the drift, measured  */
        CHECK_NEAR(m.bias[i], s.bias[i], 1e-6);                  /* and used at once     */
    }
    CHECK_NEAR(k.acc_g, 1.0, 1e-6);
    CHECK(m.bias_learned);

    /* LEVEL: left lying on the table it never completes (steady, but not picked up) */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 5.0, false);
    CHECK_INT(k.step, CALIB_LEVEL);
    CHECK(!k.picked_up && k.note[0] != 0);
    /* picked up and held tilted toward you by 25 deg, with hand tremor under 8 deg/s */
    calib_run(&k, &m, &s, &cfg, 50, 0, 0, 0.5, false);           /* top edge up 25 deg   */
    CHECK(k.picked_up);
    for (int i = 0; i < 60; i++) {                               /* ~2 s of small wobble */
        double t = (i % 2) ? 3 : -3;
        calib_run(&k, &m, &s, &cfg, t, 0, -t, 1.0 / SIM_HZ * 2, false);
    }
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_INT(k.step, CALIB_ROLL);
    CHECK_NEAR(k.level_pitch, 25, 0.5);
    CHECK_NEAR(k.level_bank, 0, 0.5);
    CHECK_NEAR(m.pitch_deg, 0, 0.5);                             /* now relative to it   */

    /* ROLL: not far enough one way, then both ways, then back to level */
    calib_run(&k, &m, &s, &cfg, 0, 0, -60, 0.5, false);          /* right 30             */
    calib_run(&k, &m, &s, &cfg, 0, 0, 60, 1.0, false);           /* to left 30           */
    calib_run(&k, &m, &s, &cfg, 0, 0, -60, 0.5, false);          /* back to level        */
    CHECK(k.reached);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_INT(k.step, CALIB_PITCH);
    CHECK_NEAR(k.full[MOTION_ROLL], 27, 1);                      /* 90 % of 30           */

    /* a reading at the sensor's limit restarts the step */
    calib_run(&k, &m, &s, &cfg, 40, 0, 0, 0.5, false);           /* up 20                */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 0.1, true);             /* clipped              */
    CHECK_INT(k.step, CALIB_PITCH);
    CHECK_NEAR(k.hi, 0, 1e-12);
    CHECK(k.note[0] != 0);
    /* PITCH: 20 up, 20 down (from the 25 deg level), back */
    calib_run(&k, &m, &s, &cfg, -40, 0, 0, 1.0, false);          /* to 20 down           */
    calib_run(&k, &m, &s, &cfg, 40, 0, 0, 1.0, false);           /* to 20 up             */
    calib_run(&k, &m, &s, &cfg, -40, 0, 0, 0.5, false);          /* back                 */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_INT(k.step, CALIB_YAW);
    CHECK_NEAR(k.full[MOTION_PITCH], 18, 1);

    /* YAW: 40 right, 40 left, back; measured without the hold-to-steer return */
    calib_run(&k, &m, &s, &cfg, 0, -80, 0, 0.5, false);          /* right 40             */
    calib_run(&k, &m, &s, &cfg, 0, 80, 0, 1.0, false);           /* to left 40           */
    calib_run(&k, &m, &s, &cfg, 0, -80, 0, 0.5, false);          /* back                 */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_INT(k.step, CALIB_DONE);
    /* held 25 deg nose-up, the twist is still measured about the face, as in flight
     * (motion_yaw.c): 40 deg, so 90 % of it is 36 (about the vertical it read 36.3) */
    CHECK_NEAR(k.full[MOTION_YAW], 36, 1e-12);
    calib_run(&k, &m, &s, &cfg, 0, 0, 30, 1.0, false);           /* done: nothing changes */
    CHECK_INT(k.step, CALIB_DONE);

    /* a clipped reading during the twist restarts the reach, but not its zero: the
     * twist still counts from where the step began */
    calib_start(&k);
    k.step = CALIB_YAW; k.min_reach = 10;
    motion_init(&m);
    sim_init(&s, 9000);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 0.2, false);
    calib_run(&k, &m, &s, &cfg, 0, -40, 0, 0.5, false);         /* 20 deg right          */
    double before_clip = k.twist;
    CHECK(before_clip > 15);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0 / SIM_HZ, true);    /* clipped               */
    CHECK(k.note[0] != 0 && k.hi == 0 && k.lo == 0);
    CHECK_NEAR(k.twist, before_clip, 1e-9);                      /* the zero stays        */

    /* a reach is kept inside the axis's range, pitch also inside what can be read */
    calib_start(&k);
    k.step = CALIB_PITCH; k.min_reach = 10; k.level_pitch = 30;
    k.hi = 50; k.lo = -50; k.reached = true; k.angle = 0;
    motion_init(&m);
    sim_init(&s, 9000);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_INT(k.step, CALIB_YAW);
    CHECK_NEAR(k.full[MOTION_PITCH], 35, 1e-12);                 /* 65 - 30, not 45       */
    calib_start(&k);                                             /* a level past 55 deg   */
    k.step = CALIB_PITCH; k.min_reach = 10; k.level_pitch = 58;  /* (LEVEL refuses it)    */
    k.hi = 50; k.lo = -50; k.reached = true; k.angle = 0;
    motion_init(&m);
    sim_init(&s, 9000);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);
    CHECK_NEAR(k.full[MOTION_PITCH], MOTION_PITCH_FULL_MIN, 1e-12);   /* never below range */

    /* LEVEL notices a pick-up by any of its signs, not only a quick turn */
    calib_to_level(&k, &m, &s, &cfg);
    CHECK_INT(k.step, CALIB_LEVEL);
    calib_run(&k, &m, &s, &cfg, 15, 0, 0, 1.0 / SIM_HZ, false);  /* one knock on the table */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 2.0, false);
    CHECK(!k.picked_up && k.tremor < 0.1);                       /* not a hand            */
    calib_to_level(&k, &m, &s, &cfg);                            /* a tilt alone: lifted  */
    calib_run(&k, &m, &s, &cfg, 0.25, 0, 0, 10.0, false);        /* very slowly, 2.5 deg  */
    CHECK(!k.picked_up);
    calib_run(&k, &m, &s, &cfg, 0.25, 0, 0, 3.0, false);         /* 3.25 deg from the table */
    CHECK(k.picked_up && k.tremor < 0.3);                        /* the tilt did it: under
                                                                    PICKUP_TREMOR_DPS     */
    calib_to_level(&k, &m, &s, &cfg);                            /* a tremor alone        */
    for (int i = 0; i < 32; i++) {                               /* 0.5 s, no net turn    */
        double t = (i % 2) ? 1.0 : -1.0;
        calib_run(&k, &m, &s, &cfg, t, 0, t, 1.0 / SIM_HZ, false);
    }
    CHECK(k.picked_up);

    /* held tilted more than 55 deg toward you: no room left to pitch, so refused */
    calib_to_level(&k, &m, &s, &cfg);
    calib_run(&k, &m, &s, &cfg, 58, 0, 0, 1.0, false);           /* nose up 58 deg        */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 3.0, false);
    CHECK_INT(k.step, CALIB_LEVEL);
    CHECK(strstr(k.note, "flatter") != NULL);
    calib_run(&k, &m, &s, &cfg, -10, 0, 0, 0.8, false);          /* 50 deg: accepted      */
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 2.5, false);
    CHECK_INT(k.step, CALIB_ROLL);
    CHECK_NEAR(k.level_pitch, 50, 0.5);

    /* gravity off 1 g: a warning that lasts to the end, alongside the steps' own hints */
    motion_init(&m);
    sim_init(&s, 7000);
    calib_start(&k);
    calib_run_raw(&k, &m, &s, &cfg, 3.3, false, 0.93);           /* a scale 7 % short     */
    CHECK_INT(k.step, CALIB_LEVEL);
    CHECK(strstr(k.warning, "0.93") != NULL);
    calib_run_raw(&k, &m, &s, &cfg, 0.5, false, 0.93);
    CHECK(k.note[0] != 0 && strstr(k.warning, "0.93") != NULL);  /* both kept            */
    calib_start(&k);
    CHECK(k.warning[0] == 0);                                    /* a new run clears it   */

    /* DSXP vibrated the controller just now: FLAT waits the half second out */
    motion_init(&m);
    sim_init(&s, 7000);
    calib_start(&k);
    calib_run_raw(&k, &m, &s, &cfg, 1.0, true, 1.0);             /* vibrating             */
    CHECK_NEAR(k.hold_s, 0, 1e-12);
    CHECK(strstr(k.note, "vibrating") != NULL);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 3.0, false);            /* 0.5 s wait, 2.5 still */
    CHECK_INT(k.step, CALIB_FLAT);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 0.6, false);
    CHECK_INT(k.step, CALIB_LEVEL);

    calib_cancel(&k);
    CHECK_INT(k.step, CALIB_IDLE);
    calib_run(&k, &m, &s, &cfg, 0, 0, 0, 1.0, false);            /* idle: ignored         */
    CHECK_INT(k.step, CALIB_IDLE);

    /* the level helpers invert each other */
    double b, p;
    motion_set_level(&m, 12.5, -7.25);
    motion_attitude(m.neutral, &b, &p);
    CHECK_NEAR(b, 12.5, 1e-9);
    CHECK_NEAR(p, -7.25, 1e-9);
}

void test_motion(void) {
    static motion_t m;
    sim_t s;
    motion_cfg_t cfg = MOTION_DEFAULTS;
    cfg.enabled = true;
    cfg.calibrated = true;                       /* motion flies only once calibrated */
    cfg.use[MOTION_YAW] = true;

    /* at rest, flat: level, nothing to fly */
    start(&m, &s, &cfg);
    CHECK(m.seeded);
    CHECK_NEAR(m.bank_deg, 0, 1e-6);
    CHECK_NEAR(m.pitch_deg, 0, 1e-6);
    for (int i = 0; i < 3; i++) CHECK_NEAR(m.out[i], 0, 1e-9);

    /* bank right 30 deg (roll right = negative gyro z), then hold 2 s */
    run(&m, &s, &cfg, 0, 0, -60, 0.5);
    run(&m, &s, &cfg, 0, 0, 0, 2.0);
    CHECK_NEAR(m.bank_deg, 30, 0.5);
    CHECK_NEAR(m.pitch_deg, 0, 0.5);
    /* output with full 40, deadzone 2: shape(30/40, 2/40) = (0.75 - 0.05) / 0.95 */
    CHECK_NEAR(m.out[MOTION_ROLL], (0.75 - 0.05) / 0.95, 0.02);
    CHECK_NEAR(m.out[MOTION_PITCH], 0, 1e-9);

    /* pitch up 10 deg, then bank right 30 deg about the controller's long axis: both read
     * back exactly (the aircraft order: pitch, then bank) */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 20, 0, 0, 0.5);            /* top edge up 10 deg (gyro x > 0)     */
    run(&m, &s, &cfg, 0, 0, -60, 0.5);           /* then bank right 30 deg              */
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK_NEAR(m.pitch_deg, 10, 0.5);
    CHECK_NEAR(m.bank_deg, 30, 0.5);

    /* a twist about the face leaves bank and pitch alone and steers right, one to one */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 0, -40, 0, 0.5);           /* twist right 20 deg (negative gyro y) */
    CHECK_NEAR(m.bank_deg, 0, 0.5);
    CHECK_NEAR(m.pitch_deg, 0, 0.5);
    CHECK_NEAR(m.yaw.deg, 20, 0.05);             /* followed while moving                 */
    CHECK(m.out[MOTION_YAW] > 0.5);

    /* held still in the hand, the rudder stays where it is for as long as the hand holds
     * it (no return to center) */
    hold_hand(&m, &s, &cfg, 30.0);
    CHECK(m.yaw.holding && !m.resting);
    CHECK_NEAR(m.yaw.deg, 20, 0.05);
    /* twisting back the full 20 deg brings it to center, without overshooting */
    double lowest = m.yaw.deg;
    for (int i = 0; i < (int)(0.5 * SIM_HZ); i++) {
        run(&m, &s, &cfg, 0, 40, 0, 1.0 / SIM_HZ);
        if (m.yaw.deg < lowest) lowest = m.yaw.deg;
    }
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK(lowest > -0.1);
    CHECK_NEAR(m.yaw.deg, 0, 0.05);
    CHECK_NEAR(m.out[MOTION_YAW], 0, 1e-9);      /* centered: no rudder                   */

    /* full deflection is reachable before the controller reads as out of range: with
     * the roll limit (65 deg) a full bank gives 1.0 (at 90 it peaked at 0.866) */
    motion_cfg_t wide = cfg;
    wide.full_deg[MOTION_ROLL] = MOTION_ROLL_FULL_MAX;
    start(&m, &s, &wide);
    run(&m, &s, &wide, 0, 0, -132, 0.5);             /* bank right 66 deg                 */
    run(&m, &s, &wide, 0, 0, 0, 1.0);
    CHECK(!m.out_of_range);
    CHECK_NEAR(m.out[MOTION_ROLL], 1.0, 1e-9);

    /* banking or pitching, whatever the level, never reads as twist: the twist is the turn
     * about the controller's face (about the vertical, 30 deg of bank at a 15.3 deg level
     * read as -7.8 deg of yaw) */
    for (int lv = 0; lv < 2; lv++) {
        double level = lv ? 25.0 : 15.3;
        start(&m, &s, &cfg);
        run(&m, &s, &cfg, level * 2, 0, 0, 0.5);     /* nose up to the level              */
        motion_set_level(&m, 0, level);
        run(&m, &s, &cfg, 0, 0, -60, 0.5);           /* bank right 30 about the long axis */
        hold_hand(&m, &s, &cfg, 3.0);
        CHECK_NEAR(m.bank_deg, 30, 0.5);
        CHECK_NEAR(m.yaw.deg, 0, 1e-9);
        CHECK_NEAR(m.out[MOTION_YAW], 0, 1e-12);
    }
    /* ... and tilting round in circles adds nothing up (about the vertical, 4 circles of
     * 10 deg built up 40.7 deg) */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 20, 0, 0, 0.5);                /* nose up 10                        */
    for (int i = 0; i < 4 * 128; i++) {              /* a 10 deg circle every 2 s, 4 times */
        double ph = 2 * M_PI * i / 128.0, a = 10 * M_PI;
        run(&m, &s, &cfg, a * cos(ph), 0, a * sin(ph), 1.0 / SIM_HZ);
    }
    CHECK_NEAR(m.yaw.deg, 0, 1e-9);

    /* set down (as still as on a table), the yaw eases back to center */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 0, -40, 0, 0.5);               /* twist right 20 deg                */
    hold_hand(&m, &s, &cfg, 1.0);
    CHECK_NEAR(m.yaw.deg, 20, 0.05);
    run(&m, &s, &cfg, 0, 0, 0, 2.5);                 /* lying still, not yet 3 s          */
    CHECK(!m.resting);
    CHECK_NEAR(m.yaw.deg, 20, 0.05);
    run(&m, &s, &cfg, 0, 0, 0, 1.5);                 /* past 3 s: set down                */
    CHECK(m.resting);
    CHECK_NEAR(m.yaw.deg, 0, 0.01);
    CHECK_NEAR(m.out[MOTION_YAW], 0, 1e-12);

    /* recenter while held tilted toward you: that becomes level; a bank from there
     * reads as a bank, not a mix */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 40, 0, 0, 0.5);            /* top edge up 20 deg                    */
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK_NEAR(m.pitch_deg, 20, 0.5);
    motion_recenter(&m);
    run(&m, &s, &cfg, 0, 0, 0, 0.1);
    CHECK_NEAR(m.pitch_deg, 0, 0.1);
    run(&m, &s, &cfg, 0, 0, -20, 0.5);           /* bank right 10 deg                     */
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK_NEAR(m.bank_deg, 10, 0.5);
    CHECK_NEAR(m.pitch_deg, 0, 0.5);

    /* invert flips the sign; a full angle at or below zero flies nothing */
    motion_cfg_t inv = cfg;
    inv.invert[MOTION_ROLL] = true;
    run(&m, &s, &inv, 0, 0, 0, 1.0 / SIM_HZ);
    CHECK(m.out[MOTION_ROLL] < 0);
    inv.full_deg[MOTION_ROLL] = 0;
    run(&m, &s, &inv, 0, 0, 0, 1.0 / SIM_HZ);
    CHECK_NEAR(m.out[MOTION_ROLL], 0, 1e-12);

    /* gyro bias: learned while still, not while DSXP vibrates the controller */
    start(&m, &s, &cfg);
    s.bias[0] = -0.6; s.bias[1] = -0.85; s.bias[2] = 0.9;
    motion_sample_t smp;
    for (int i = 0; i < (int)(3 * SIM_HZ); i++) {     /* 3 s of our own rumble            */
        double z[3] = { 0, 0, 0 };
        sim_step(&s, z, &smp);
        motion_update(&m, &smp, &cfg, true);
    }
    CHECK(!m.bias_learned);
    run(&m, &s, &cfg, 0, 0, 0, 0.4);             /* still inside the 0.5 s hold-off      */
    CHECK(!m.bias_learned);
    run(&m, &s, &cfg, 0, 0, 0, 15.0);
    CHECK(m.bias_learned);
    for (int i = 0; i < 3; i++) CHECK_NEAR(m.bias[i], s.bias[i], 0.1);
    CHECK_NEAR(m.bank_deg, 0, 0.5);              /* the drift did not tilt "level"      */

    /* a slow, steady 2 deg/s turn is a turn, not drift */
    double before = m.bias[1];
    run(&m, &s, &cfg, 0, -2.0, 0, 10.0);
    CHECK_NEAR(m.bias[1], before, 1e-9);

    /* the sensor clock wraps around 2^32 without a hiccup */
    start(&m, &s, &cfg);
    s.ts = 0xFFFFFFFFu - 100000u;
    run(&m, &s, &cfg, 0, 0, 0, 0.05);            /* re-seeds once (big jump), then normal */
    long n0 = m.samples;
    run(&m, &s, &cfg, 0, 0, -60, 0.25);          /* crosses the wrap mid-turn             */
    CHECK(s.ts < 0x10000000u);
    CHECK_INT(m.samples - n0, 16);
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK_NEAR(m.bank_deg, 15, 0.5);

    /* a 0.2 s gap is a break: re-seeded from the accelerometer; the held yaw is kept (the
     * hand barely moves in it), and only a gap over YAW_GAP_KEEP_S centers it */
    run(&m, &s, &cfg, 0, -40, 0, 0.25);
    double before_gap = m.yaw.deg;
    CHECK(before_gap > 5);
    s.ts += (uint32_t)(0.2 * MOTION_TICKS_PER_SEC);
    run(&m, &s, &cfg, 0, 0, 0, 1.0 / SIM_HZ);
    CHECK_NEAR(m.yaw.deg, before_gap, 1e-12);
    CHECK_NEAR(m.bank_deg, 15, 0.5);
    s.ts += (uint32_t)(1.5 * MOTION_TICKS_PER_SEC);
    run(&m, &s, &cfg, 0, 0, 0, 1.0 / SIM_HZ);
    CHECK_NEAR(m.yaw.deg, 0, 1e-12);

    /* the same timestamp twice is a duplicate: ignored */
    long n1 = m.samples;
    smp.ts = s.ts;
    for (int i = 0; i < 3; i++) { smp.gyro_dps[i] = 500; smp.acc_g[i] = s.R[1][i]; }
    motion_update(&m, &smp, &cfg, false);
    CHECK_INT(m.samples, n1);
    CHECK_NEAR(m.bank_deg, 15, 0.5);

    /* face down, or stood on its edge: out of range, every axis neutral */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 0, 0, 180, 1.0);           /* roll over onto its face               */
    run(&m, &s, &cfg, 0, 0, 0, 1.0);
    CHECK(m.out_of_range);
    for (int i = 0; i < 3; i++) CHECK_NEAR(m.out[i], 0, 1e-12);
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 80, 0, 0, 1.0);            /* top edge up 80 deg                    */
    run(&m, &s, &cfg, 0, 0, 0, 0.5);
    CHECK(m.out_of_range);
    CHECK_NEAR(m.out[MOTION_PITCH], 0, 1e-12);
    run(&m, &s, &cfg, -80, 0, 0, 1.0);           /* back to flat: flying again           */
    run(&m, &s, &cfg, 0, 0, 0, 0.5);
    CHECK(!m.out_of_range);

    /* an accelerometer reading of zero cannot seed the filter: nothing happens */
    motion_init(&m);
    motion_sample_t zero = { { 0, 0, 0 }, { 0, 0, 0 }, 5 };
    motion_update(&m, &zero, &cfg, false);
    CHECK(!m.seeded);
    for (int i = 0; i < 3; i++) CHECK_NEAR(m.out[i], 0, 1e-12);

    /* after a gap, a jolted sample (1.28 g, sideways) does not seed "up" (it used to:
     * 39 deg of bank on a level controller); the next calm one does, held twist kept */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 0, -40, 0, 0.25);              /* 10 deg of twist, held             */
    double held_twist = m.yaw.deg;
    s.ts += (uint32_t)(0.2 * MOTION_TICKS_PER_SEC);  /* a 0.2 s gap ...                   */
    motion_sample_t jolt = { { 0, 0, 0 }, { 0.8, 1.0, 0 }, s.ts };
    motion_update(&m, &jolt, &cfg, false);           /* ... then a jolt                   */
    CHECK(!m.seeded);
    run(&m, &s, &cfg, 0, 0, 0, 1.0 / SIM_HZ);        /* calm, level                       */
    CHECK(m.seeded);
    CHECK_NEAR(m.bank_deg, 0, 0.5);
    CHECK_NEAR(m.yaw.deg, held_twist, 1e-9);
    /* a smaller jolt within 0.25 g still seeds, bent, and fades in a tenth of a second
     * (it took a second at the normal correction) */
    s.ts += (uint32_t)(0.2 * MOTION_TICKS_PER_SEC);
    motion_sample_t nudge = { { 0, 0, 0 }, { 0.5, 0.9, 0 }, s.ts };
    motion_update(&m, &nudge, &cfg, false);
    CHECK(m.seeded && m.bank_deg < -20);
    run(&m, &s, &cfg, 0, 0, 0, 0.25);
    CHECK_NEAR(m.bank_deg, 0, 2.5);

    /* a reconnect keeps the captured level and the bias, and restarts the rest */
    start(&m, &s, &cfg);
    m.neutral[0] = 0.1; m.bias[2] = 0.5; m.yaw.deg = 12;
    motion_reset(&m);
    CHECK(!m.seeded && m.yaw.deg == 0 && m.neutral[0] == 0.1 && m.bias[2] == 0.5);

    /* the owner test used everywhere */
    CHECK(motion_owns(&cfg, MOTION_ROLL));
    cfg.enabled = false;
    CHECK(!motion_owns(&cfg, MOTION_ROLL));
    CHECK(!motion_owns(&MOTION_DEFAULTS, MOTION_YAW));
    cfg.enabled = true; cfg.calibrated = false;
    CHECK(!motion_owns(&cfg, MOTION_ROLL));              /* not before the calibration */
    CHECK(!motion_owns(&cfg, 7));

    /* crossfade: smoothstep from the last value sent to the new target over 0.3 s */
    motion_blend_t b = { false, 0, 0 };
    CHECK_NEAR(motion_blend(&b, 0.25, 10.0), 0.25, 1e-12);     /* idle: the target   */
    motion_blend_start(&b, 0.8, 10.0);
    CHECK_NEAR(motion_blend(&b, 0.0, 10.0), 0.8, 1e-12);
    CHECK_NEAR(motion_blend(&b, 0.0, 10.15), 0.4, 1e-9);        /* halfway             */
    CHECK_NEAR(motion_blend(&b, 0.0, 10.3), 0.0, 1e-12);
    CHECK(!b.active);
    motion_blend_start(&b, 0.5, 10.0);
    CHECK_NEAR(motion_blend(&b, 0.1, 9.0), 0.1, 1e-12);         /* clock went back     */
    CHECK(!b.active);
}

/* ------------------------------------------------------------------ yaw on its own */

/* Feed the yaw `secs` of a constant twist rate (deg/s, right positive) at 64 Hz, the clock
 * running on from *t. Returns the largest change of the yaw flown from one sample to the
 * next. */
static double yaw_run(motion_yaw_t *y, double *t, double rate, double secs, const motion_cfg_t *cfg) {
    double dt = 1.0 / SIM_HZ, step = 0;
    double g[3] = { 0, -rate, 0 }, b[3] = { 0, 0, 0 };   /* twist right = negative gyro y */
    for (int i = 0; i < (int)lround(secs * SIM_HZ); i++) {
        double before = y->deg;
        *t += dt;
        motion_yaw_update(y, g, b, dt, *t, cfg);
        step = fmax(step, fabs(y->deg - before));
    }
    return step;
}

/* A slow twist of `deg` at `rate`, held 1 s, then a quick twist back to the start: the
 * yaw reached, and where it ends (it should end at the start). */
static void slow_and_back(double deg, double rate, double *reached, double *ended) {
    static motion_yaw_t y;
    motion_cfg_t cfg = MOTION_DEFAULTS;
    double t = 0;
    motion_yaw_reset(&y);
    yaw_run(&y, &t, 0, 1.0, &cfg);
    yaw_run(&y, &t, rate, deg / rate, &cfg);
    yaw_run(&y, &t, 0, 1.0, &cfg);
    *reached = y.deg;
    yaw_run(&y, &t, -40, deg / 40, &cfg);
    yaw_run(&y, &t, 0, 1.0, &cfg);
    *ended = y.deg;
}

/* motion_yaw.c: follow, hold, drift, adding up, the sudden override, set down, gaps. */
void test_motion_yaw(void) {
    static motion_yaw_t y;
    motion_cfg_t cfg = MOTION_DEFAULTS;
    double t = 0;

    /* the twist rate: the bias-removed turn about the face, twist right positive */
    double g[3] = { 7, -40, 9 }, b0[3] = { 0, 0, 0 };
    CHECK_NEAR(motion_yaw_rate(g, b0), 40, 1e-12);              /* bank and pitch: none   */
    CHECK_NEAR(motion_yaw_rate(g, g), 0, 1e-12);                /* all of it was bias    */

    /* follow: one to one while the hand moves */
    motion_yaw_reset(&y);
    yaw_run(&y, &t, 40, 0.5, &cfg);
    CHECK(!y.holding);
    CHECK_NEAR(y.deg, 20, 1e-9);

    /* hold: steady for YAW_STEADY_S, the yaw keeps the window's average and stays */
    yaw_run(&y, &t, 0, YAW_STEADY_S + 0.1, &cfg);
    CHECK(y.holding);
    double held = y.deg;
    CHECK_NEAR(held, 20, 0.05);
    double step = yaw_run(&y, &t, 0, 30.0, &cfg);               /* half a minute still    */
    CHECK(y.holding && step < 1e-3);
    CHECK_NEAR(y.deg, held, 0.01);

    /* tremor (back and forth, no net twist) does not move a held yaw */
    for (int i = 0; i < 128; i++) yaw_run(&y, &t, (i % 2) ? 3 : -3, 1.0 / SIM_HZ, &cfg);
    CHECK(y.holding);
    CHECK_NEAR(y.deg, held, 0.01);

    /* drift slower than YAW_CREEP_DPS is taken out while held: a minute at 0.1 deg/s
     * moves nothing, and the drift it measured is subtracted from then on */
    yaw_run(&y, &t, 0.1, 60.0, &cfg);
    CHECK(y.holding);
    CHECK_NEAR(y.deg, held, 0.01);
    CHECK(y.drift_dps > 0.05 && y.drift_dps < 0.1);

    /* a bump that goes and comes back (0.2 deg out in 0.1 s, then back) does not let go */
    yaw_run(&y, &t, 2, 0.1, &cfg);
    yaw_run(&y, &t, -2, 0.1, &cfg);
    yaw_run(&y, &t, 0, 0.3, &cfg);
    CHECK(y.holding);
    CHECK_NEAR(y.deg, held, 0.01);

    /* one 100 ms window of change alone does not let go either */
    yaw_run(&y, &t, 1.5, YAW_FAST_S, &cfg);
    CHECK(y.holding);
    yaw_run(&y, &t, 0, 1.0, &cfg);

    /* faster than 1 deg/s, the same way in two windows in a row: let go at once and follow
     * the twist one to one */
    CHECK(y.holding);
    double before = y.raw;
    yaw_run(&y, &t, 1.5, YAW_FAST_WINDOWS * YAW_FAST_S + 0.05, &cfg);
    CHECK(!y.holding);
    yaw_run(&y, &t, 1.5, 2.0, &cfg);
    CHECK(!y.holding);
    CHECK_NEAR(y.deg, y.raw, 0.01);                             /* following, caught up  */

    /* drift is taken out while following too: with 0.1 deg/s of real drift on top of a
     * steady 1.5 deg/s twist for 10 s, the twist reads 15 deg, not 16 */
    motion_yaw_reset(&y);
    yaw_run(&y, &t, 0.1, 180.0, &cfg);                          /* learn it while held   */
    CHECK_NEAR(y.drift_dps, 0.1, 0.01);
    before = y.raw;
    yaw_run(&y, &t, 1.6, 10.0, &cfg);                           /* 1.5 twist + 0.1 drift */
    CHECK(!y.holding);
    CHECK_NEAR(y.raw - before, 15.0, 0.15);
    /* a new bias means the drift is measured afresh */
    double g2[3] = { 0, 0, 0 }, b2[3] = { 0, 0.3, 0 };
    motion_yaw_update(&y, g2, b2, 1.0 / SIM_HZ, t += 1.0 / SIM_HZ, &cfg);
    CHECK(y.drift_dps == 0);

    /* a slow deliberate twist counts in full, and twisting back to the start ends there
     * (taking 0.25 deg/s off every slow twist reached 3.96 of 10 deg and ended at -5.21) */
    double reached, ended;
    slow_and_back(10, 0.5, &reached, &ended);
    CHECK(reached > 8.8 && reached < 10.01);
    CHECK_NEAR(ended, 0, 0.2);
    slow_and_back(10, 0.3, &reached, &ended);
    CHECK(reached > 8.8 && reached < 10.01);
    CHECK_NEAR(ended, 0, 0.2);
    /* ... and smoothly: every step eased */
    motion_yaw_reset(&y);
    yaw_run(&y, &t, 0, 1.0, &cfg);
    step = yaw_run(&y, &t, 0.5, 10.0, &cfg);
    CHECK(step < 0.2);

    /* the output: through the yaw deadzone, inverted if asked */
    motion_yaw_reset(&y);
    y.drift_dps = 0;
    yaw_run(&y, &t, 40, 0.25, &cfg);                            /* 10 deg right          */
    double want = shape(10.0 / cfg.full_deg[MOTION_YAW], cfg.yaw_deadzone_deg / cfg.full_deg[MOTION_YAW], 1.0);
    CHECK_NEAR(motion_yaw_output(&y, &cfg), want, 1e-9);
    motion_cfg_t inv = cfg;
    inv.invert[MOTION_YAW] = true;
    CHECK_NEAR(motion_yaw_output(&y, &inv), -want, 1e-9);

    /* a reading that is not a number changes nothing (it must never fly full rudder) */
    double nan_g[3] = { 0, NAN, 0 }, deg_before = y.deg, raw_before = y.raw;
    motion_yaw_update(&y, nan_g, b0, 1.0 / SIM_HZ, t += 1.0 / SIM_HZ, &cfg);
    CHECK(y.deg == deg_before && y.raw == raw_before);
    CHECK_NEAR(motion_yaw_output(&y, &cfg), want, 1e-9);

    /* gaps: a short one keeps the yaw, a long (or unknown) one centers it */
    motion_yaw_gap(&y, 0.5);
    CHECK(y.deg == deg_before);
    motion_yaw_gap(&y, 1.5);
    CHECK(y.deg == 0 && y.raw == 0 && !y.holding);
    yaw_run(&y, &t, 40, 0.25, &cfg);
    motion_yaw_gap(&y, NAN);
    CHECK(y.deg == 0);

    /* set down: the yaw eases (no step) to center, and stays there */
    yaw_run(&y, &t, 40, 0.25, &cfg);
    motion_yaw_rest(&y);
    step = yaw_run(&y, &t, 0, 1.0, &cfg);
    CHECK(y.holding && step < 2.0);                             /* 10 deg, eased         */
    CHECK_NEAR(y.deg, 0, 1e-3);

    /* no endless wind-up: the twist stops at YAW_WINDUP times its full angle */
    motion_yaw_reset(&y);
    yaw_run(&y, &t, 100, 2.0, &cfg);
    CHECK_NEAR(y.raw, YAW_WINDUP * cfg.full_deg[MOTION_YAW], 1e-9);
    CHECK_NEAR(motion_yaw_output(&y, &cfg), 1.0, 1e-9);

    /* reset: center, following, no history; the measured drift (the gyro's) stays */
    y.drift_dps = 0.05;
    motion_yaw_reset(&y);
    CHECK(y.deg == 0 && y.raw == 0 && !y.holding && y.hist_n == 0 && y.drift_dps == 0.05);
}
