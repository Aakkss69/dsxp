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

/* Start a fusion at rest, flat, with one sample to seed it. */
static void start(motion_t *m, sim_t *s, const motion_cfg_t *cfg) {
    motion_init(m);
    sim_init(s, 1000);
    run(m, s, cfg, 0, 0, 0, 0.1);
}

void test_motion(void) {
    static motion_t m;
    sim_t s;
    motion_cfg_t cfg = MOTION_DEFAULTS;
    cfg.enabled = true;
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

    /* a twist about the face leaves bank and pitch alone and steers right */
    start(&m, &s, &cfg);
    run(&m, &s, &cfg, 0, -40, 0, 0.5);           /* twist right 20 deg (negative gyro y) */
    CHECK_NEAR(m.bank_deg, 0, 0.5);
    CHECK_NEAR(m.pitch_deg, 0, 0.5);
    CHECK(m.yaw_deg > 15 && m.yaw_deg < 20);     /* the return already took a little     */
    CHECK(m.out[MOTION_YAW] > 0.3);

    /* hold-to-steer returns to center and never overshoots on the way back */
    run(&m, &s, &cfg, 0, 0, 0, 4.0);             /* held: it eases back                  */
    CHECK(m.yaw_deg > 0 && m.yaw_deg < 6);
    double lowest = m.yaw_deg;
    for (int i = 0; i < (int)(0.5 * SIM_HZ); i++) {   /* twist back the full 20 deg      */
        run(&m, &s, &cfg, 0, 40, 0, 1.0 / SIM_HZ);
        if (m.yaw_deg < lowest) lowest = m.yaw_deg;
    }
    CHECK(lowest > -1e-9);
    CHECK_NEAR(m.yaw_deg, 0, 1e-6);
    CHECK_NEAR(m.out[MOTION_YAW], 0, 1e-9);

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

    /* a 0.2 s gap is a break: re-seeded from the accelerometer, held yaw dropped */
    run(&m, &s, &cfg, 0, -40, 0, 0.25);
    CHECK(m.yaw_deg > 5);
    s.ts += (uint32_t)(0.2 * MOTION_TICKS_PER_SEC);
    run(&m, &s, &cfg, 0, 0, 0, 1.0 / SIM_HZ);
    CHECK_NEAR(m.yaw_deg, 0, 1e-12);
    CHECK_NEAR(m.bank_deg, 15, 0.5);

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

    /* a reconnect keeps the captured level and the bias, and restarts the rest */
    start(&m, &s, &cfg);
    m.neutral[0] = 0.1; m.bias[2] = 0.5; m.yaw_deg = 12;
    motion_reset(&m);
    CHECK(!m.seeded && m.yaw_deg == 0 && m.neutral[0] == 0.1 && m.bias[2] == 0.5);

    /* the owner test used everywhere */
    CHECK(motion_owns(&cfg, MOTION_ROLL));
    cfg.enabled = false;
    CHECK(!motion_owns(&cfg, MOTION_ROLL));
    CHECK(!motion_owns(&MOTION_DEFAULTS, MOTION_YAW));
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
