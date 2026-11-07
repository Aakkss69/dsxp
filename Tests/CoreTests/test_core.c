/* Core behavior with a fake clock and captured packets: the gate, the override
 * planner, input staleness, telemetry, wind vs manual triggers, saves, output. */
#include "tests.h"
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { unsigned char d[520]; size_t n; } pkt_t;
static pkt_t P[8192];
static int NP, OUTN;
static unsigned char OUTLAST[OUT_REPORT_LEN];

static void cap_udp(void *ctx, const unsigned char *b, size_t n) {
    (void)ctx;
    if (NP < 8192) { memcpy(P[NP].d, b, n < 520 ? n : 520); P[NP].n = n; NP++; }
}
static void cap_out(void *ctx, const unsigned char *r) { (void)ctx; OUTN++; memcpy(OUTLAST, r, OUT_REPORT_LEN); }
static void reset_cap(void) { NP = OUTN = 0; }

/* how many DREFs to `path` were sent, and the last value */
static int drefs(const char *path, float *last) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (P[i].n == DREF_LEN && !memcmp(P[i].d, "DREF", 4) && !strcmp((char *)P[i].d + 9, path)) {
            n++;
            if (last) memcpy(last, P[i].d + 5, 4);
        }
    return n;
}
static int drefs_with(const char *path, float v) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (P[i].n == DREF_LEN && !memcmp(P[i].d, "DREF", 4) && !strcmp((char *)P[i].d + 9, path)) {
            float f; memcpy(&f, P[i].d + 5, 4);
            if (f == v) n++;
        }
    return n;
}
static int rrefs(int32_t freq) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (P[i].n == RREF_LEN && !memcmp(P[i].d, "RREF", 4)) {
            int32_t f; memcpy(&f, P[i].d + 5, 4);
            if (f == freq) n++;
        }
    return n;
}
static int cmnds(const char *cmd) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (!memcmp(P[i].d, "CMND", 4) && P[i].n == 5 + strlen(cmd) + 1 && !memcmp(P[i].d + 5, cmd, strlen(cmd))) n++;
    return n;
}

static void feed(core_t *c, int lx, int b0, double now) {
    unsigned char r[78];
    memset(r, 0, sizeof r);
    r[0] = 0x31;
    r[OFF_LX] = (unsigned char)lx; r[OFF_LY] = 128; r[OFF_RX] = 128; r[OFF_RY] = 128;
    r[OFF_BTN0] = (unsigned char)b0;
    r[OFF_TOUCH0] = r[OFF_TOUCH1] = 0x80;
    core_on_report(c, r, 78, now);
}

static void udp_pairs(core_t *c, const int32_t *ids, const float *vals, int n, bool loop, double now) {
    unsigned char b[5 + 8 * 32];
    memcpy(b, "RREF,", 5);
    for (int i = 0; i < n; i++) { memcpy(b + 5 + 8 * i, &ids[i], 4); memcpy(b + 9 + 8 * i, &vals[i], 4); }
    core_on_udp(c, b, 5 + 8 * (size_t)n, loop, now);
}

/* X-Plane answering: one loopback RREF reply makes telemetry live (the gate needs it) */
static void tele(core_t *c, double now) {
    int32_t ids[] = { 1 };
    float vals[] = { 0.0f };
    udp_pairs(c, ids, vals, 1, true, now);
}

static const core_sinks_t SK = { NULL, cap_udp, cap_out };
#define OV_ROLL  "sim/operation/override/override_joystick_roll"
#define OV_PITCH "sim/operation/override/override_joystick_pitch"
#define OV_HDG   "sim/operation/override/override_joystick_heading"
#define OV_BRK   "sim/operation/override/override_toe_brakes"
#define DR_ROLL  "sim/joystick/yoke_roll_ratio"
#define DR_LBRK  "sim/cockpit2/controls/left_brake_ratio"
#define DR_RBRK  "sim/cockpit2/controls/right_brake_ratio"
#define DR_THR   "sim/cockpit2/engine/actuators/throttle_ratio_all"

static core_t C;

static void fresh(void) {
    settings_t s;
    load_defaults(&s);
    core_init(&C, &s, &SK);
    reset_cap();
}

static void gate_and_overrides(void) {
    fresh();
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    CHECK(!C.engaged);                               /* no input yet: stay out */
    feed(&C, 128, 0x08, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    CHECK(!C.engaged);                               /* X-Plane not answering yet (F34) */
    tele(&C, 1.0);
    CHECK(C.force_gate);                             /* first reply re-runs the gate */
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    CHECK(C.engaged);
    core_tick(&C, 1.0);
    float v = 0;
    CHECK_INT(drefs(OV_ROLL, &v), 1);  CHECK_NEAR(v, 1.0, 0);
    CHECK_INT(drefs(OV_PITCH, NULL), 1);
    CHECK_INT(drefs(OV_HDG, NULL), 1);               /* yaw + differential rudder */
    CHECK_INT(drefs(OV_BRK, NULL), 0);               /* no brake axis mapped */

    /* re-asserted once a second, not every tick */
    reset_cap();
    feed(&C, 128, 0x08, 1.5); core_tick(&C, 1.5);
    CHECK_INT(drefs(OV_ROLL, NULL), 0);
    feed(&C, 128, 0x08, 2.05); core_tick(&C, 2.05);
    CHECK_INT(drefs(OV_ROLL, NULL), 1);

    /* unmapping roll while engaged hands roll straight back to X-Plane */
    reset_cap();
    cmd_t ax = { .kind = CMD_AXIS, .key = S_LX };
    ax.u.axis = (axis_map_t){ AX_NONE, false };
    core_apply_cmd(&C, &ax, 2.2);
    feed(&C, 255, 0x08, 2.2); core_tick(&C, 2.2);
    CHECK_INT(drefs_with(DR_ROLL, 0.0f), 1);
    CHECK_INT(drefs_with(OV_ROLL, 0.0f), 1);
    for (double t = 3.25; t < 7.0; t += 1.05) { feed(&C, 255, 0x08, t); core_tick(&C, t); }
    CHECK_INT(drefs_with(OV_ROLL, 0.0f), 1 + OVERRIDE_RELEASE_REPEAT);
    CHECK_INT(drefs(DR_ROLL, NULL), 1);              /* no more roll values after the center */
    CHECK_INT(drefs_with(OV_ROLL, 1.0f), 0);

    /* USB plugged in: X-Plane's native path wins, overrides released twice */
    reset_cap();
    core_set_gate(&C, true, true, true, "X-Plane", 7.0);
    CHECK(!C.engaged);
    CHECK_INT(drefs_with(OV_PITCH, 0.0f), 2);
    CHECK_INT(drefs_with(DR_LBRK, 0.0f), 2);         /* brakes let go too (F6) */
    CHECK_INT(drefs_with(DR_RBRK, 0.0f), 2);

    /* releases repeat while X-Plane runs, only for overrides that were on (F2) */
    reset_cap();
    for (double t = 8.05; t < 12.5; t += 1.05) core_tick(&C, t);
    CHECK_INT(drefs_with(OV_PITCH, 0.0f), OVERRIDE_RELEASE_REPEAT);
    CHECK_INT(drefs_with(OV_HDG, 0.0f), OVERRIDE_RELEASE_REPEAT);
    CHECK_INT(drefs_with(OV_ROLL, 0.0f), 0);         /* roll was already handed back */
    CHECK_INT(drefs_with(OV_BRK, 0.0f), 0);          /* brakes were never overridden */

    /* a disengage while X-Plane quits schedules nothing further */
    fresh();
    feed(&C, 128, 0x08, 1.0); tele(&C, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    core_set_gate(&C, true, false, false, "", 1.5);
    CHECK(!C.engaged);
    reset_cap();
    for (double t = 2.6; t < 6.0; t += 1.05) core_tick(&C, t);
    CHECK_INT(drefs_with(OV_PITCH, 0.0f), 0);
}

static void staleness(void) {
    fresh();
    feed(&C, 128, 0x08, 10.0); tele(&C, 10.0);
    core_set_gate(&C, true, false, true, "X-Plane", 10.0);
    CHECK(C.engaged);
    reset_cap();
    feed(&C, 255, 0x08 | 0x20, 10.0);               /* stick hard over, Cross held */
    core_tick(&C, 10.0);
    float v = 0;
    drefs(DR_ROLL, &v);
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK_INT(cmnds("sim/engines/throttle_down"), 1);
    reset_cap();
    core_tick(&C, 10.0 + INPUT_STALE_SEC + 0.05);   /* no new report */
    drefs(DR_ROLL, &v);
    CHECK_NEAR(v, 0.0, 0);                          /* held neutral, not replayed */
    CHECK_INT(cmnds("sim/engines/throttle_down"), 0);
    CHECK(C.prev_btn[B_CROSS]);                     /* kept: a gap is not a release */
    core_tick(&C, 10.0 + INPUT_LOST_SEC + 0.1);
    CHECK(C.force_gate);                            /* gate re-runs immediately */
    core_set_gate(&C, true, false, true, "X-Plane", 10.0 + INPUT_LOST_SEC + 0.1);
    CHECK(!C.engaged);
}

/* A button held through a Bluetooth gap, or while DSXP disengages and engages again,
 * fires its one-shot X-Plane command once (a held gear toggle used to fire 3 times). */
#define GEAR "sim/flight_controls/landing_gear_toggle"
static void button_gaps(void) {
    fresh();
    snprintf(C.s.bind[B_CROSS].cmd, CMD_PATH_MAX, "%s", GEAR);
    C.s.bind[B_CROSS].repeat = false;
    feed(&C, 128, 0x08, 10.0); tele(&C, 10.0);
    core_set_gate(&C, true, false, true, "X-Plane", 10.0);
    CHECK(C.engaged);
    reset_cap();
    feed(&C, 128, 0x08 | 0x20, 10.1); core_tick(&C, 10.1);      /* Cross pressed          */
    CHECK_INT(cmnds(GEAR), 1);
    core_tick(&C, 10.1 + INPUT_STALE_SEC + 0.05);                 /* a gap, still held      */
    feed(&C, 128, 0x08 | 0x20, 10.5); core_tick(&C, 10.5);        /* reports back, held     */
    CHECK_INT(cmnds(GEAR), 1);
    core_set_gate(&C, true, false, false, "", 10.6);              /* X-Plane gone ...       */
    CHECK(!C.engaged);
    feed(&C, 128, 0x08 | 0x20, 10.7); tele(&C, 10.7);
    core_set_gate(&C, true, false, true, "X-Plane", 10.7);        /* ... and back, held     */
    CHECK(C.engaged);
    core_tick(&C, 10.7);
    CHECK_INT(cmnds(GEAR), 1);
    feed(&C, 128, 0x08, 10.8); core_tick(&C, 10.8);               /* released ...           */
    feed(&C, 128, 0x08 | 0x20, 10.9); core_tick(&C, 10.9);        /* ... pressed again      */
    CHECK_INT(cmnds(GEAR), 2);
}

/* Toe brakes: the override takes both brakes, so with only L2 on the left brake the
 * right one is written too (released), never left frozen; xp_release_now sends every
 * pending release at once (before an X-Plane port change). */
static void brakes_and_releases(void) {
    fresh();
    for (int i = 0; i < S_COUNT; i++) C.s.axis[i].target = AX_NONE;
    C.s.axis[S_L2].target = AX_BRAKE_L;
    feed(&C, 128, 0x08, 10.0); tele(&C, 10.0);
    core_set_gate(&C, true, false, true, "X-Plane", 10.0);
    reset_cap();
    core_tick(&C, 10.0);
    float v = 1;
    CHECK(drefs(DR_LBRK, &v) >= 1);
    CHECK(drefs(DR_RBRK, &v) >= 1);
    CHECK_NEAR(v, 0, 1e-12);                                      /* right: released        */
    core_set_gate(&C, true, false, false, "", 10.1);              /* disengaged: releases   */
    CHECK(C.ov_release_left[OV_BRAKES_I] > 0);                    /*   pending, spread out  */
    reset_cap();
    xp_release_now(&C);
    CHECK_INT(drefs_with(OV_BRK, 0.0f), C.bridge.override_release_repeats);
    CHECK_INT(C.ov_release_left[OV_BRAKES_I], 0);
}

static void telemetry_and_triggers(void) {
    fresh();
    /* manual Resist 128 on L2, then wind mode */
    cmd_t t = { .kind = CMD_TRIGGER, .key = 0 };
    t.u.trig.v = (trig_manual_t){ TFX_RESIST, 30, 0, 128, 40 };
    t.u.trig.mask = TF_MODE | TF_START | TF_END | TF_STRENGTH;
    core_apply_cmd(&C, &t, 1.0);
    CHECK_INT(C.fx_out[0].strength, 128);
    cmd_t w = { .kind = CMD_TRIGGER_SOURCE }; w.u.source = TRIGGER_SRC_WIND;
    core_apply_cmd(&C, &w, 1.0);

    core_set_gate(&C, false, false, true, "X-Plane", 1.0);   /* sim up, no controller */
    core_tick(&C, 1.0);
    CHECK_INT(rrefs(TELEMETRY_HZ), telemetry_rref_count());   /* every dataref subscribed */

    int32_t ids[] = { 1 };
    float vals[] = { 999.0f };
    udp_pairs(&C, ids, vals, 1, false, 1.1);         /* not from loopback: ignored */
    CHECK(!C.wx.live);
    vals[0] = 100.0f;
    udp_pairs(&C, ids, vals, 1, true, 1.1);
    CHECK(C.wx.live);
    CHECK_NEAR(C.wx.ias, 100.0, 1e-6);
    core_tick(&C, 1.1);
    CHECK_INT(C.fx_out[0].mode, TFX_RESIST);
    CHECK(C.fx_out[0].strength != 128);              /* wind computes its own */
    CHECK_INT(C.s.fx_manual[0].strength, 128);       /* ...and never touches the manual one */
    const trig_manual_t *fm = &C.s.fx_manual[0];
    CHECK_INT(fm->mode, TFX_RESIST); CHECK_INT(fm->start, 30); CHECK_INT(fm->end, 0);
    CHECK_INT(fm->strength, 128); CHECK_INT(fm->freq, 40);
    w.u.source = TRIGGER_SRC_MANUAL;
    core_apply_cmd(&C, &w, 1.2);
    CHECK_INT(C.fx_out[0].strength, 128);

    /* telemetry stops: everything the simulator set is released */
    cmd_t rm = { .kind = CMD_RUMBLE_SOURCE }; rm.u.source = RUMBLE_SRC_SIM;
    core_apply_cmd(&C, &rm, 1.2);
    int32_t ids2[] = { RR_CRASH, RR_STALL0, RR_STALL0 + 1, RR_STALL0 + 2 };
    float vals2[] = { 1.0f, 1.0f, 1.0f, 1.0f };       /* crashed, left wing stalled */
    udp_pairs(&C, ids2, vals2, 4, true, 1.3);
    core_tick(&C, 1.3);
    CHECK_INT(C.rumble.left, 255);
    CHECK_NEAR(C.wx.stall_l, 1.0, 1e-6);
    core_tick(&C, 1.3 + TELEMETRY_TIMEOUT_SEC + 0.1);
    CHECK(!C.wx.live);
    CHECK_INT(C.rumble.left, 0);
    CHECK(!C.wx.crashed);
    CHECK_NEAR(C.stall_el[0][0] + C.stall_el[0][1] + C.stall_el[0][2], 0.0, 0);
    CHECK(!strcmp(C.wx.trigger_note, "Telemetry lost"));
}

static void light_sim_alone(void) {
    fresh();
    C.s.light.source = LIGHT_SRC_SIM;                          /* only the light bar follows the simulator */
    core_set_gate(&C, false, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    CHECK(rrefs(TELEMETRY_HZ) > 0);
    int32_t ids[] = { 1 };
    float vals[] = { 80.0f };
    udp_pairs(&C, ids, vals, 1, true, 1.1);
    core_tick(&C, 1.1);
    CHECK(!strcmp(C.light_note, "Clean"));

    /* X-Plane quits: unsubscribed (frequency 0) */
    reset_cap();
    core_set_gate(&C, false, false, false, "", 2.0);
    core_tick(&C, 2.0);
    CHECK_INT(rrefs(0), telemetry_rref_count());
}

static void saves_and_output(void) {
    fresh();
    cmd_t tn = { .kind = CMD_TUNE };
    tn.u.tune.has_dz = true; tn.u.tune.dz = 0.2;
    core_apply_cmd(&C, &tn, 20.0);
    CHECK_INT(core_save_due(&C, 20.4), 0);           /* debounced by timing.save_delay */
    CHECK_INT(core_save_due(&C, 20.6), CFB_CONTROLS); /* only the file that changed */
    CHECK_INT(core_save_due(&C, 20.7), 0);
    static config_t cf;
    core_get_config(&C, &cf);
    CHECK_NEAR(cf.s.tune.deadzone, 0.2, 1e-12);
    cmd_t lt = { .kind = CMD_LIGHT };
    lt.u.light.mask = LF_ON;
    core_apply_cmd(&C, &lt, 20.8);
    core_apply_cmd(&C, &tn, 20.9);
    CHECK_INT(core_save_due(&C, 21.5), CFB_CONTROLS | CFB_FEEDBACK);

    /* an outside edit arrives through core_apply_config: nothing marked for saving,
     * and a mode change goes through the same setter as the mapper's */
    core_get_config(&C, &cf);
    cf.s.tune.curve = 2.0;
    cf.s.trigger_source = TRIGGER_SRC_WIND;
    cf.bridge.telemetry_hz = 20;
    core_apply_config(&C, &cf, CFB_CONTROLS | CFB_FEEDBACK, 21.6);
    CHECK_NEAR(C.s.tune.curve, 2.0, 1e-12);
    CHECK_INT(C.s.trigger_source, TRIGGER_SRC_WIND);
    CHECK(!strcmp(C.wx.trigger_note, "Waiting for X-Plane"));
    CHECK_NEAR(C.bridge.telemetry_hz, 10, 1e-12);     /* bridge.conf was not in the mask */
    CHECK_INT(core_save_due(&C, 30.0), 0);
    cf.s.trigger_source = TRIGGER_SRC_MANUAL;
    core_apply_config(&C, &cf, CFB_FEEDBACK | CFB_BRIDGE, 21.7);
    CHECK_INT(C.s.trigger_source, TRIGGER_SRC_MANUAL);
    CHECK_NEAR(C.bridge.telemetry_hz, 20, 1e-12);
    C.bridge = BRIDGE_DEFAULTS;

    /* output reports go out only with a device, then quiet on demand */
    core_tick(&C, 31.0);
    CHECK_INT(OUTN, 0);
    core_on_device(&C, true, true);
    core_tick(&C, 31.1);
    CHECK(OUTN >= 1);
    out_quiet(&C);
    CHECK_INT(OUTLAST[3], 0x0F);
    CHECK_INT(OUTLAST[5], 0); CHECK_INT(OUTLAST[6], 0);
    CHECK_INT(OUTLAST[47], 0);
    core_on_device(&C, true, false);
    CHECK(!C.have_report);
}

/* F3, F4, F5: X-Plane 12 telemetry as it is published */
static void xp12_telemetry(void) {
    float t[WX_LAYERS] = { 0, 4, 8 }, a[WX_LAYERS] = { 0, 1000, 2000 };
    for (int i = 3; i < WX_LAYERS; i++) { t[i] = 8; a[i] = 2000 + 1000.0f * (float)i; }
    CHECK_NEAR(turb_at_altitude(t, a, WX_LAYERS, 1500), 6.0, 1e-9);
    CHECK_NEAR(turb_at_altitude(t, a, WX_LAYERS, -50), 0.0, 1e-9);        /* below: lowest layer */
    CHECK_NEAR(turb_at_altitude(t, a, WX_LAYERS, 99999), 8.0, 1e-9);      /* above: highest */
    float tu[3] = { 8, 0, 4 }, au[3] = { 2000, 0, 1000 };                  /* unsorted input */
    CHECK_NEAR(turb_at_altitude(tu, au, 3, 500), 2.0, 1e-9);
    float tz[3] = { 3, 9, 9 }, az[3] = { 0, 0, 0 };                        /* altitudes not filled */
    CHECK_NEAR(turb_at_altitude(tz, az, 3, 1500), 3.0, 1e-9);
    float tn[2] = { NAN, 5 }, an[2] = { 0, 100 };
    CHECK_NEAR(turb_at_altitude(tn, an, 2, 10), 5.0, 1e-9);               /* NaN layer skipped */

    char p[128];
    CHECK(telemetry_rref_path(RR_STALL0 + 1 * STALL_SAMPLES + 2, p, sizeof p));   /* wing 1, element 8 */
    CHECK(!strcmp(p, "sim/flightmodel2/wing/elements/element_is_stalled[18]"));
    CHECK(telemetry_rref_path(RR_TURB0 + 12, p, sizeof p));
    CHECK(!strcmp(p, "sim/weather/aircraft/turbulence[12]"));
    CHECK(!telemetry_rref_path(0, p, sizeof p) && !telemetry_rref_path(RR_COUNT, p, sizeof p));
    for (int id = 1; id < RR_COUNT; id++) CHECK(telemetry_rref_path(id, p, sizeof p) && strlen(p) < 400);
    CHECK_INT(telemetry_rref_count(), 47);

    /* turbulence follows the aircraft through the layers */
    fresh();
    int32_t lid[7] = { RR_TURB0, RR_TURB0 + 1, RR_TURB0 + 2, RR_ALT0, RR_ALT0 + 1, RR_ALT0 + 2, RR_ELEV };
    float lv[7] = { 0, 4, 8, 0, 1000, 2000, 1500 };
    udp_pairs(&C, lid, lv, 7, true, 1.0);
    CHECK_NEAR(C.wx.turb, 6.0, 1e-6);

    /* stall on Plane Maker wing 2 (outer left) counts as the left wing */
    int32_t sid[3] = { RR_STALL0 + 2 * STALL_SAMPLES, RR_STALL0 + 2 * STALL_SAMPLES + 1, RR_STALL0 + 2 * STALL_SAMPLES + 2 };
    float sv[3] = { 1, 1, 1 };
    udp_pairs(&C, sid, sv, 3, true, 1.1);
    CHECK_NEAR(C.wx.stall_l, 1.0, 1e-6);
    CHECK_NEAR(C.wx.stall_r, 0.0, 1e-6);
    int32_t rid[1] = { RR_STALL0 + 3 * STALL_SAMPLES };                  /* wing 3 = right */
    float rv[1] = { 1 };
    udp_pairs(&C, rid, rv, 1, true, 1.2);
    CHECK_NEAR(C.wx.stall_r, 1.0 / 3, 1e-6);

    /* paused: sim rumble and wind triggers let go, still engaged; resume brings them back */
    fresh();
    C.s.rumble_source = RUMBLE_SRC_SIM; C.s.trigger_source = TRIGGER_SRC_WIND;
    feed(&C, 128, 0x08, 1.0);
    int32_t cid[2] = { RR_CRASH, RR_IAS };
    float cv[2] = { 1, 120 };
    udp_pairs(&C, cid, cv, 2, true, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    CHECK_INT(C.rumble.left, 255);
    CHECK(C.fx_out[0].mode != TFX_OFF);
    int32_t pid[1] = { RR_PAUSED };
    float pv[1] = { 1 };
    udp_pairs(&C, pid, pv, 1, true, 1.1);
    feed(&C, 128, 0x08, 1.1);
    core_tick(&C, 1.1);
    CHECK_INT(C.rumble.left, 0);
    CHECK_INT(C.fx_out[0].mode, TFX_OFF);
    CHECK(C.engaged);
    CHECK(!strcmp(C.rumble_note, "Paused"));
    pv[0] = 0;
    udp_pairs(&C, pid, pv, 1, true, 1.2);
    feed(&C, 128, 0x08, 1.2);
    core_tick(&C, 1.2);
    CHECK_INT(C.rumble.left, 255);
    CHECK(C.fx_out[0].mode != TFX_OFF);
}

/* F7: throttle has no override, so it is sent on change plus a slow keepalive */
static void throttle(void) {
    fresh();
    C.s.axis[S_RY] = (axis_map_t){ AX_THROTTLE, false };
    feed(&C, 128, 0x08, 1.0); tele(&C, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    reset_cap();
    for (double t = 1.0; t < 2.0; t += 1.0 / 60) { feed(&C, 128, 0x08, t); tele(&C, t); core_tick(&C, t); }
    CHECK_INT(drefs(DR_THR, NULL), 1);               /* a still stick: sent once */
    C.state.raw01[S_RY] = 0.9;                       /* stick moves */
    C.last_report_at = 2.0;
    core_tick(&C, 2.0);
    CHECK_INT(drefs(DR_THR, NULL), 2);
    for (double t = 2.0; t < 2.0 + THROTTLE_KEEPALIVE_SEC + 0.5; t += 0.5) {
        C.last_report_at = t; tele(&C, t); core_tick(&C, t);
    }
    CHECK_INT(drefs(DR_THR, NULL), 3);               /* one keepalive */
}

static void suspend_and_dry_run(void) {
    fresh();
    feed(&C, 128, 0x08, 1.0); tele(&C, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    CHECK(C.engaged);
    core_set_suspended(&C, true, 1.1);
    CHECK(!C.engaged);
    core_set_gate(&C, true, false, true, "X-Plane", 1.1);
    CHECK(!C.engaged);
    core_set_suspended(&C, false, 1.2);
    CHECK(C.force_gate);
    feed(&C, 128, 0x08, 1.2);
    core_set_gate(&C, true, false, true, "X-Plane", 1.2);
    CHECK(C.engaged);

    fresh();
    C.dry_run = true;
    feed(&C, 128, 0x08, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    CHECK(!C.engaged);
    CHECK_INT(NP, 0);                                /* not a single packet to X-Plane */
}

static int rm_entry(const char *p, const struct stat *s, int flag, struct FTW *f) {
    (void)s; (void)flag; (void)f;
    return remove(p);
}

static bool wait_status(bool (*ok)(const dsxp_status_t *), dsxp_status_t *st) {
    for (int i = 0; i < 200; i++) {               /* up to 2 s */
        dsxp_get_status(st);
        if (ok(st)) return true;
        usleep(10000);
    }
    return false;
}
static bool st_ticking(const dsxp_status_t *s) { return s->seq > 3; }
static bool st_suspended(const dsxp_status_t *s) { return s->suspended; }
static bool st_resumed(const dsxp_status_t *s) { return !s->suspended; }

/* The real runtime (threads, run loop, queue, snapshot), without IOKit HID and
 * without talking to X-Plane: start, suspend, resume, stop, restart (F1, F21). */
static void runtime_smoke(void) {
    const char *tmp = getenv("TMPDIR");
    char root[1024];
    snprintf(root, sizeof root, "%s/dsxp-smoke-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) { CHECK(!"mkdtemp"); return; }
    char data[1100], logs[1100];
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(logs, sizeof logs, "%s/logs", root);

    runtime_test_disable_hid = true;
    dsxp_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.data_dir = data;
    cfg.log_dir = logs;
    cfg.commands_path = "";
    cfg.dry_run = true;
    dsxp_status_t st;
    CHECK_INT(dsxp_start(&cfg), 0);
    CHECK(dsxp_is_running());
    CHECK(wait_status(st_ticking, &st));
    dsxp_set_suspended(true);
    CHECK(wait_status(st_suspended, &st));
    dsxp_set_suspended(false);
    CHECK(wait_status(st_resumed, &st));
    CHECK(dsxp_stop(2.0));
    CHECK(!dsxp_is_running());
    CHECK(access(data, F_OK) == 0);
    CHECK_INT(dsxp_start(&cfg), 0);                  /* restart after a clean stop */
    CHECK(wait_status(st_ticking, &st));
    CHECK(dsxp_stop(NAN));                            /* NaN timeout is clamped, not UB */

    char longp[1100];
    memset(longp, 'a', sizeof longp - 1);
    longp[0] = '/';
    longp[sizeof longp - 1] = 0;
    cfg.data_dir = longp;
    CHECK_INT(dsxp_start(&cfg), -1);                  /* would be truncated: refused (F19) */
    CHECK(!dsxp_is_running());
    runtime_test_disable_hid = false;
    nftw(root, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* What the guard sends after a crash: centered yoke, brakes off, all four
 * overrides off, and a frequency-0 RREF for every telemetry id (F6, F25). */
static void guard_packets(void) {
    reset_cap();
    guard_build_release(cap_udp, NULL);
    CHECK_INT(drefs_with(DR_ROLL, 0.0f), 1);
    CHECK_INT(drefs_with(DR_LBRK, 0.0f), 1);
    CHECK_INT(drefs_with(DR_RBRK, 0.0f), 1);
    CHECK_INT(drefs_with(OV_ROLL, 0.0f) + drefs_with(OV_PITCH, 0.0f) + drefs_with(OV_HDG, 0.0f) +
              drefs_with(OV_BRK, 0.0f), 4);
    CHECK_INT(rrefs(0), telemetry_rref_count());
    CHECK_INT(rrefs(TELEMETRY_HZ), 0);
}

/* ------------------------------------------------------------ motion control */

#define DR_PITCH "sim/joystick/yoke_pitch_ratio"
#define DR_HDG   "sim/joystick/yoke_heading_ratio"

static uint32_t MTS = 1000;          /* simulated sensor clock, 64 Hz steps */

/* A report with sticks centered except LX, the triggers, one button byte, and the motion
 * sensors: accelerometer for a controller banked `bank` deg right (nominal scale, so
 * 8191.75 counts per g), gyro still. */
static void feed_m(core_t *c, int lx, int l2, int r2, int b0, int b1, double bank, double now) {
    unsigned char r[78];
    memset(r, 0, sizeof r);
    r[0] = 0x31;
    r[OFF_LX] = (unsigned char)lx; r[OFF_LY] = 128; r[OFF_RX] = 128; r[OFF_RY] = 128;
    r[OFF_L2] = (unsigned char)l2; r[OFF_R2] = (unsigned char)r2;
    r[OFF_BTN0] = (unsigned char)b0; r[OFF_BTN1] = (unsigned char)b1;
    r[OFF_TOUCH0] = r[OFF_TOUCH1] = 0x80;
    double g = MOTION_ACC_PER_G_NOMINAL, rad = bank * M_PI / 180.0;
    int ax = (int)lround(-sin(rad) * g), ay = (int)lround(cos(rad) * g);   /* banked right: up leans -x */
    r[OFF_ACCEL]     = (unsigned char)(ax & 0xFF); r[OFF_ACCEL + 1] = (unsigned char)((ax >> 8) & 0xFF);
    r[OFF_ACCEL + 2] = (unsigned char)(ay & 0xFF); r[OFF_ACCEL + 3] = (unsigned char)((ay >> 8) & 0xFF);
    MTS += 46875;                                          /* 1/64 s of 0.33 us ticks */
    for (int k = 0; k < 4; k++) r[OFF_SENSOR_TS + k] = (unsigned char)(MTS >> (8 * k));
    core_on_report(c, r, 78, now);
}

/* Start a core with motion on (roll and pitch by motion, yaw by sticks unless asked),
 * left stick X still mapped to roll, engaged, with the controller banked `bank`. */
static void motion_fresh(bool yaw_motion, double bank) {
    settings_t s;
    load_defaults(&s);
    s.motion.enabled = true;
    s.motion.calibrated = true;                           /* as after the calibration */
    s.motion.use[MOTION_YAW] = yaw_motion;
    core_init(&C, &s, &SK);
    reset_cap();
    feed_m(&C, 128, 0, 0, 0x08, 0, bank, 1.0);
    tele(&C, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
}

static void motion_control(void) {
    float v = 0;
    double want = shape(20.0 / 40.0, 2.0 / 40.0, 1.0);   /* 20 deg bank with the defaults */

    /* motion flies roll: the stick on roll is ignored, the override is asserted, and the
     * value sent is the controller's bank */
    motion_fresh(false, 20);
    CHECK(C.engaged);
    feed_m(&C, 255, 0, 0, 0x08, 0, 20, 1.02);             /* full right stick on roll       */
    core_tick(&C, 1.02);
    CHECK_INT(drefs_with(OV_ROLL, 1.0f), 1);
    CHECK(drefs(DR_ROLL, &v) == 1);
    CHECK_NEAR(v, want, 1e-4);
    CHECK(C.sent_valid);
    CHECK_NEAR(C.sent[MOTION_ROLL], want, 1e-4);
    CHECK_NEAR(C.sent[MOTION_PITCH], 0, 1e-6);

    /* the roll override is wanted for motion even with no stick on roll */
    settings_t s;
    load_defaults(&s);
    s.axis[S_LX].target = AX_NONE;
    bool w[OV_COUNT];
    xp_wanted_overrides(&s, true, w);
    CHECK(!w[OV_ROLL_I]);
    s.motion.enabled = true;
    xp_wanted_overrides(&s, true, w);
    CHECK(!w[OV_ROLL_I]);                                 /* not calibrated: still not wanted */
    s.motion.calibrated = true;
    xp_wanted_overrides(&s, true, w);
    CHECK(w[OV_ROLL_I] && w[OV_PITCH_I]);
    s.motion.use[MOTION_YAW] = true;
    s.axis[S_RX].target = s.axis[S_L2].target = s.axis[S_R2].target = AX_NONE;
    xp_wanted_overrides(&s, true, w);
    CHECK(w[OV_HEADING_I]);
    xp_wanted_overrides(&s, false, w);                    /* calibrating: the sticks fly,  */
    CHECK(!w[OV_ROLL_I] && !w[OV_HEADING_I]);             /*   so unmapped axes go back    */

    /* ... and so in flight: calibrating with no stick on roll releases its override
     * instead of pinning roll at 0 under it */
    motion_fresh(false, 20);
    C.s.axis[S_LX].target = AX_NONE;
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.02);
    core_tick(&C, 1.02);
    CHECK(drefs_with(OV_ROLL, 1.0f) >= 1);
    cmd_t cal_go = { .kind = CMD_CALIBRATE, .key = CALIB_ACTION_START };
    core_apply_cmd(&C, &cal_go, 1.05);
    reset_cap();
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.1);
    core_tick(&C, 1.1);
    CHECK(drefs_with(OV_ROLL, 0.0f) >= 1);                /* handed back to X-Plane        */
    reset_cap();
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.2);
    core_tick(&C, 1.2);
    float rv = 0;
    CHECK_INT(drefs(DR_ROLL, &rv), 0);                    /* nothing pins roll any more    */
    /* ... and the routine's level cannot be moved by a Recenter meanwhile */
    double lv[3];
    memcpy(lv, C.motion.neutral, sizeof lv);
    cmd_t rc = { .kind = CMD_MOTION_RECENTER };
    core_apply_cmd(&C, &rc, 1.25);
    CHECK(lv[0] == C.motion.neutral[0] && lv[1] == C.motion.neutral[1] && lv[2] == C.motion.neutral[2]);

    /* Recenter is refused when the controller cannot be a level: face down (it used to
     * make level upside down, then full aileron held level) or tilted past 55 deg */
    motion_fresh(false, 0);
    memcpy(lv, C.motion.neutral, sizeof lv);
    double down[3] = { 0, -1, 0 }, tilted[3] = { -0.866, 0.5, 0 }, banked[3] = { -0.5, 0.866, 0 };
    memcpy(C.motion.up, down, sizeof down);
    C.motion.out_of_range = true;
    CHECK(!core_motion_recenter(&C, 1.3));
    memcpy(C.motion.up, tilted, sizeof tilted);           /* banked 60 deg                 */
    C.motion.out_of_range = false;
    CHECK(!core_motion_recenter(&C, 1.3));
    CHECK(lv[0] == C.motion.neutral[0] && lv[1] == C.motion.neutral[1] && lv[2] == C.motion.neutral[2]);
    memcpy(C.motion.up, banked, sizeof banked);           /* banked 30 deg: a fine level   */
    CHECK(core_motion_recenter(&C, 1.3));
    CHECK_NEAR(C.motion.neutral[0], -0.5, 1e-12);

    /* full deflection stays reachable from the level: level + full within 65 deg */
    motion_cfg_t room = MOTION_DEFAULTS;
    room.level_pitch_deg = 30; room.full_deg[MOTION_PITCH] = 60;
    room.level_bank_deg = -20; room.full_deg[MOTION_ROLL] = 65;
    CHECK(motion_full_fix(&room, MOTION_PITCH) && motion_full_fix(&room, MOTION_ROLL));
    CHECK_NEAR(room.full_deg[MOTION_PITCH], 35, 1e-12);
    CHECK_NEAR(room.full_deg[MOTION_ROLL], 45, 1e-12);
    room.level_pitch_deg = 15.3; room.full_deg[MOTION_PITCH] = 60;
    motion_full_fix(&room, MOTION_PITCH);
    CHECK_NEAR(room.full_deg[MOTION_PITCH], 49, 1e-12);   /* whole degrees: 65 - 15.3      */

    /* yaw by motion: the rudder triggers are ignored (nothing twisted: yaw 0) */
    motion_fresh(true, 0);
    feed_m(&C, 128, 0, 255, 0x08, 0, 0, 1.02);            /* right rudder trigger floored   */
    core_tick(&C, 1.02);
    CHECK(drefs(DR_HDG, &v) == 1);
    CHECK_NEAR(v, 0, 1e-6);
    motion_fresh(false, 0);                               /* yaw by sticks: the trigger works */
    feed_m(&C, 128, 0, 255, 0x08, 0, 0, 1.02);
    core_tick(&C, 1.02);
    CHECK(drefs(DR_HDG, &v) == 1);
    CHECK_NEAR(v, 1.0, 1e-6);

    /* stale input: every axis neutral, motion included */
    motion_fresh(false, 20);
    reset_cap();
    core_tick(&C, 1.5);                                   /* 0.5 s with no report          */
    CHECK(drefs(DR_ROLL, &v) == 1);
    CHECK_NEAR(v, 0, 1e-6);

    /* a button bound to a DSXP command acts in DSXP and never reaches X-Plane */
    motion_fresh(false, 20);
    snprintf(C.s.bind[B_CROSS].cmd, CMD_PATH_MAX, "%s", DSXP_CMD_RECENTER);
    reset_cap();
    feed_m(&C, 128, 0, 0, 0x28, 0, 20, 1.1);              /* cross down (0x20) + dpad idle */
    core_tick(&C, 1.1);
    CHECK_INT(cmnds(DSXP_CMD_RECENTER), 0);
    CHECK(C.lamp_blinking);                               /* confirmed on the player lamps */
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.2);              /* released                       */
    core_tick(&C, 1.2);
    CHECK_NEAR(C.motion.bank_deg, 0, 1e-6);               /* held at 20 deg: now level      */
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.6);
    core_tick(&C, 1.6);                                   /* crossfade over, blink over      */
    CHECK(!C.lamp_blinking);
    CHECK_NEAR(C.sent[MOTION_ROLL], 0, 1e-6);

    /* ... also while standing by (not engaged) */
    motion_fresh(false, 20);
    core_set_gate(&C, true, false, false, "", 1.05);      /* X-Plane gone                  */
    CHECK(!C.engaged && !C.sent_valid);
    CHECK_NEAR(C.sent[MOTION_ROLL], 0, 1e-12);
    snprintf(C.s.bind[B_CROSS].cmd, CMD_PATH_MAX, "%s", DSXP_CMD_RECENTER);
    feed_m(&C, 128, 0, 0, 0x28, 0, 20, 1.1);
    core_tick(&C, 1.1);
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.2);
    CHECK_NEAR(C.motion.bank_deg, 0, 1e-6);

    /* toggle: flips motion.enabled and saves controls.conf like the mapper does */
    motion_fresh(false, 0);
    snprintf(C.s.bind[B_SQUARE].cmd, CMD_PATH_MAX, "%s", DSXP_CMD_TOGGLE);
    C.cfg_dirty_mask = 0;
    feed_m(&C, 128, 0, 0, 0x18, 0, 0, 1.1);               /* square down (0x10)             */
    core_tick(&C, 1.1);
    CHECK(!C.s.motion.enabled);
    CHECK(C.cfg_dirty_mask & CFB_CONTROLS);
    feed_m(&C, 128, 0, 0, 0x18, 0, 0, 1.12);              /* still held: no second toggle   */
    core_tick(&C, 1.12);
    CHECK(!C.s.motion.enabled);
    core_tick(&C, 1.12 + INPUT_STALE_SEC + 0.05);         /* a Bluetooth gap, still held    */
    feed_m(&C, 128, 0, 0, 0x18, 0, 0, 1.5);
    core_tick(&C, 1.5);
    CHECK(!C.s.motion.enabled);                           /* one press, one toggle          */

    /* the clutch held through a gap stays held (no recenter); once the input is lost
     * it lets go, without recentering on an attitude nobody read */
    motion_fresh(false, 20);
    snprintf(C.s.bind[B_L1].cmd, CMD_PATH_MAX, "%s", DSXP_CMD_HOLD);
    feed_m(&C, 128, 0, 0, 0x08, 0x01, 20, 1.1);
    core_tick(&C, 1.1);
    CHECK(C.motion_hold);
    double clutch_lvl = C.motion.neutral[0];
    core_tick(&C, 1.1 + INPUT_STALE_SEC + 0.05);
    CHECK(C.motion_hold);
    feed_m(&C, 128, 0, 0, 0x08, 0x01, 20, 1.45);
    core_tick(&C, 1.45);
    CHECK(C.motion_hold);
    core_tick(&C, 1.45 + INPUT_LOST_SEC + 0.1);
    CHECK(!C.motion_hold);
    CHECK_NEAR(C.motion.neutral[0], clutch_lvl, 1e-12);   /* not recentered                 */

    /* hold: the controls freeze while held; on release the pose is level and the
     * controls fade from the frozen value */
    motion_fresh(false, 20);
    snprintf(C.s.bind[B_L1].cmd, CMD_PATH_MAX, "%s", DSXP_CMD_HOLD);
    feed_m(&C, 128, 0, 0, 0x08, 0x01, 20, 1.1);           /* L1 down                        */
    core_tick(&C, 1.1);
    CHECK(C.motion_hold);
    feed_m(&C, 128, 0, 0, 0x08, 0x01, -10, 1.15);         /* re-grip: banked left now       */
    for (int i = 0; i < 60; i++) feed_m(&C, 128, 0, 0, 0x08, 0x01, -10, 1.15);
    core_tick(&C, 1.2);
    CHECK_NEAR(C.sent[MOTION_ROLL], want, 1e-4);          /* still the frozen 20 deg        */
    feed_m(&C, 128, 0, 0, 0x08, 0, -10, 1.25);            /* released                       */
    core_tick(&C, 1.25);
    CHECK(!C.motion_hold);
    CHECK(C.sent[MOTION_ROLL] > 0.4);                     /* fading from the frozen value   */
    feed_m(&C, 128, 0, 0, 0x08, 0, -10, 1.6);
    core_tick(&C, 1.6);
    CHECK_NEAR(C.sent[MOTION_ROLL], 0, 1e-3);             /* -10 deg is the new level       */

    /* switching an axis to motion mid-flight fades from the stick's value */
    settings_t s2;
    load_defaults(&s2);
    s2.motion.calibrated = true;
    core_init(&C, &s2, &SK);                              /* motion off                      */
    feed_m(&C, 255, 0, 0, 0x08, 0, 0, 1.0);
    tele(&C, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    CHECK_NEAR(C.sent[MOTION_ROLL], 1.0, 1e-6);           /* full right stick                */
    cmd_t mc = { .kind = CMD_MOTION };
    mc.u.motion.has_enabled = true; mc.u.motion.enabled = true;
    core_apply_cmd(&C, &mc, 1.05);
    feed_m(&C, 255, 0, 0, 0x08, 0, 0, 1.05);
    core_tick(&C, 1.05);
    CHECK(C.sent[MOTION_ROLL] > 0.95);                    /* starts at the stick's value    */
    feed_m(&C, 255, 0, 0, 0x08, 0, 0, 1.2);
    core_tick(&C, 1.2);
    CHECK(C.sent[MOTION_ROLL] > 0.1 && C.sent[MOTION_ROLL] < 0.9);   /* fading               */
    feed_m(&C, 255, 0, 0, 0x08, 0, 0, 1.4);
    core_tick(&C, 1.4);
    CHECK_NEAR(C.sent[MOTION_ROLL], 0, 1e-6);             /* motion: level controller        */
    CHECK(C.cfg_dirty_mask & CFB_CONTROLS);

    /* CMD_MOTION clamps to the documented ranges and past the deadzone */
    mc = (cmd_t){ .kind = CMD_MOTION };
    mc.u.motion.full_mask = 7;
    mc.u.motion.full[MOTION_ROLL] = 500; mc.u.motion.full[MOTION_PITCH] = 1; mc.u.motion.full[MOTION_YAW] = 4.4;
    mc.u.motion.use_mask = 1u << MOTION_YAW; mc.u.motion.use[MOTION_YAW] = true;
    mc.u.motion.inv_mask = 1u << MOTION_PITCH; mc.u.motion.inv[MOTION_PITCH] = true;
    core_apply_cmd(&C, &mc, 2.0);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_ROLL], MOTION_ROLL_FULL_MAX, 1e-12);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_PITCH], 10, 1e-12);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_YAW], 5, 1e-12);       /* 4.4 -> 4 -> range min 5; deadzone 4 + 1 */
    CHECK(C.s.motion.use[MOTION_YAW] && C.s.motion.invert[MOTION_PITCH]);
    CHECK(C.s.motion.use[MOTION_ROLL]);                          /* not in the mask: kept   */
    C.cfg_dirty_mask = 0;                                        /* the same values again:  */
    core_apply_cmd(&C, &mc, 2.1);                                /*   nothing to save       */
    CHECK_INT(C.cfg_dirty_mask, 0);
    mc.u.motion.full[MOTION_ROLL] = 60;                          /* a slider moved: saved   */
    core_apply_cmd(&C, &mc, 2.2);
    CHECK(C.cfg_dirty_mask & CFB_CONTROLS);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_ROLL], 60, 1e-12);

    /* switching on (here through a config edit) restores the calibrated level, dropping
     * any Recenter: held at 15 deg with level saved at 10 deg reads 5 deg */
    motion_fresh(false, 15);
    C.s.motion.enabled = false;                           /* as if it was off              */
    core_tick(&C, 1.02);
    core_motion_recenter(&C, 1.02);                       /* a temporary level at 15 deg   */
    static config_t cfg;
    core_get_config(&C, &cfg);
    cfg.s.motion.enabled = true;
    cfg.s.motion.level_bank_deg = 10;
    core_apply_config(&C, &cfg, CFB_CONTROLS, 1.05);
    feed_m(&C, 128, 0, 0, 0x08, 0, 15, 1.06);
    core_tick(&C, 1.06);
    feed_m(&C, 128, 0, 0, 0x08, 0, 15, 1.08);
    CHECK_NEAR(C.motion.bank_deg, 5, 0.01);                 /* accel counts are whole numbers */
    /* and not at all before the calibration */
    motion_fresh(false, 0);
    C.s.motion.calibrated = false;
    C.s.motion.enabled = false;
    cmd_t on = { .kind = CMD_MOTION };
    on.u.motion.has_enabled = true; on.u.motion.enabled = true;
    core_apply_cmd(&C, &on, 1.1);
    CHECK(!C.s.motion.enabled);

    /* the controller going away restarts the filter but keeps level and calibration */
    motion_cal_t cal;
    motion_cal_nominal(&cal);
    cal.factory_axes = 6;
    core_set_motion_cal(&C, &cal, MCAL_OK);
    CHECK_INT(C.motion_cal_src, MOTION_CAL_FACTORY);
    double n1 = C.motion.neutral[0];
    core_on_device(&C, true, false);
    CHECK(!C.motion.seeded);
    CHECK_NEAR(C.motion.neutral[0], n1, 1e-12);
    CHECK_INT(C.motion_cal_src, MOTION_CAL_FACTORY);
    core_set_motion_cal(&C, &cal, MCAL_BAD_CRC);
    CHECK_INT(C.motion_cal_src, MOTION_CAL_NOMINAL);

    /* calibration through the core: while it runs the sticks fly motion's axes too (faded
     * over), DSXP's own vibration is silenced, the DSXP buttons do nothing, and finishing
     * saves the results and lets motion fly again */
    motion_fresh(false, 20);
    core_on_device(&C, true, true);                       /* output reports can go out      */
    C.s.rumble_source = RUMBLE_SRC_MANUAL;
    C.rumble.left = 200;                                  /* buzzing, and L2 in Vibrate      */
    C.s.fx_manual[0] = (trig_manual_t){ TFX_VIBRATE, 2, 30, 200, 40 };
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.02);
    core_tick(&C, 1.02);
    CHECK(C.sent[MOTION_ROLL] > 0.4);
    CHECK_INT(OUTLAST[6], 200);                           /* rumble left (OFF_RUMBLE_L)      */
    CHECK_INT(C.fx_out[0].mode, TFX_VIBRATE);
    cmd_t cal_cmd = { .kind = CMD_CALIBRATE, .key = CALIB_ACTION_START };
    core_apply_cmd(&C, &cal_cmd, 1.05);
    CHECK(core_calibrating(&C));
    feed_m(&C, 255, 0, 0, 0x08, 0, 20, 1.5);              /* full right stick on roll        */
    core_tick(&C, 1.5);
    CHECK_NEAR(C.sent[MOTION_ROLL], 1.0, 1e-6);           /* the stick flies roll now        */
    CHECK_INT(OUTLAST[6], 0);                             /* the rumble is silenced          */
    unsigned char off[11];
    put_effect(off, &(trigger_fx_t){ TFX_OFF, 0, 0, 0 });
    CHECK(memcmp(OUTLAST + 24, off, sizeof off) == 0);    /* L2 (OFF_TRIG_L) not vibrating   */
    C.calib.bias[0] = -0.6573;                            /* rounded to the file's 3 places: */
    C.calib.bias[1] = -0.8004;                            /*   -0.657, -0.800, and -0.000    */
    C.calib.bias[2] = -0.0004;
    C.calib.level_bank = 20.04; C.calib.level_pitch = 6.36;
    C.calib.full[MOTION_ROLL] = 33; C.calib.full[MOTION_PITCH] = 21; C.calib.full[MOTION_YAW] = 10;
    C.s.motion.yaw_deadzone_deg = 12;                     /* deadzone past the reach         */
    C.calib.step = CALIB_DONE;
    C.cfg_dirty_mask = 0;
    core_calib_finish(&C, 1.6);
    CHECK(!core_calibrating(&C) && C.s.motion.calibrated);
    CHECK(C.cfg_dirty_mask & CFB_CONTROLS);
    CHECK_NEAR(C.s.motion.level_bank_deg, 20.0, 1e-12);          /* rounded as the file writes */
    CHECK_NEAR(C.s.motion.level_pitch_deg, 6.4, 1e-12);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_PITCH], 21, 1e-12);
    CHECK_NEAR(C.s.motion.full_deg[MOTION_YAW], 13, 1e-12);      /* 12 + 1, like config_validate */
    CHECK_NEAR(C.calib.full[MOTION_YAW], 13, 1e-12);             /* and the sheet shows that    */
    CHECK(C.s.motion.gyro_bias_dps[0] == -0.657);                /* exactly the file's value    */
    CHECK_NEAR(C.s.motion.gyro_bias_dps[1], -0.8, 1e-12);
    CHECK_NEAR(C.motion.bias[1], -0.8, 1e-12);
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 2.0);
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 2.0);
    core_tick(&C, 2.0);
    CHECK_NEAR(C.motion.bank_deg, 0, 0.1);                       /* held at the new level */
    CHECK_INT(OUTLAST[6], 200);                                  /* vibration back        */

    /* controls.conf written and read back unchanged is not an edit: the held twist and the
     * learned drift stay (each saved value is exactly what the file holds, -0.000 too) */
    static config_t disk;
    static char text[64 * 1024];
    static conf_warns_t warns;
    core_get_config(&C, &disk);
    size_t len = conf_format(CF_CONTROLS, &disk, text, sizeof text);
    CHECK(len > 0 && len < sizeof text);
    config_defaults(&disk);
    CHECK(conf_parse(CF_CONTROLS, text, len, &disk, &warns));
    CHECK(disk.s.motion.gyro_bias_dps[0] == C.s.motion.gyro_bias_dps[0]);
    C.motion.yaw.deg = 7;                                        /* a held twist          */
    C.motion.bias[2] = 0.25;                                     /* refined since         */
    core_apply_config(&C, &disk, CFB_CONTROLS, 2.05);
    core_tick(&C, 2.05);
    CHECK_NEAR(C.motion.yaw.deg, 7, 1e-12);
    CHECK_NEAR(C.motion.bias[2], 0.25, 1e-12);
    /* ... while an edited level is applied, faded in rather than jumping */
    double before = C.sent[MOTION_ROLL];
    disk.s.motion.level_bank_deg = 30;                           /* held at 20: now -10    */
    core_apply_config(&C, &disk, CFB_CONTROLS, 2.1);
    core_tick(&C, 2.1);                                          /* applied, fade started  */
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 2.11);
    core_tick(&C, 2.11);
    CHECK_NEAR(C.motion.bank_deg, -10, 0.1);
    CHECK(fabs(C.sent[MOTION_ROLL] - before) < 0.01);            /* eased, not a step      */
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 2.5);
    core_tick(&C, 2.5);
    CHECK_NEAR(C.sent[MOTION_ROLL], shape(-10.0 / 33.0, 2.0 / 33.0, 1.0), 1e-3);

    /* the confirmation flash shows on the player lamps even with the light switched off */
    C.s.light.on = false;
    core_motion_recenter(&C, 2.6);
    core_tick(&C, 2.6);
    CHECK(C.lamp_blinking);
    CHECK_INT(OUTLAST[46], 0x1F);                                /* OFF_PLAYER: all five   */
    CHECK_INT(OUTLAST[47], 0);                                   /* OFF_RED: the bar dark  */
    /* close is refused before the first calibration, and abandoning a re-run restores */
    motion_fresh(false, 0);
    C.s.motion.calibrated = false;
    core_apply_cmd(&C, &cal_cmd, 1.1);
    cmd_t close_cmd = { .kind = CMD_CALIBRATE, .key = CALIB_ACTION_CLOSE };
    core_apply_cmd(&C, &close_cmd, 1.2);
    CHECK(core_calibrating(&C));
    C.s.motion.calibrated = true;
    C.motion.bias[0] = 3;                                        /* changed by the run    */
    core_apply_cmd(&C, &close_cmd, 1.3);
    CHECK(!core_calibrating(&C));
    CHECK_NEAR(C.motion.bias[0], C.s.motion.gyro_bias_dps[0], 1e-12);

    /* the runtime's hand-over: a result is applied only for the connection it was read
     * for; one from an earlier connection is dropped (core_thread.c runtime_cal_drain) */
    motion_fresh(false, 0);
    CHECK(!runtime_cal_drain(&C));                         /* nothing posted              */
    cal.factory_axes = 6;
    runtime_cal_post(runtime_dev_gen() - 1, &cal, MCAL_OK, 0);
    CHECK(!runtime_cal_drain(&C));                         /* stale: dropped              */
    CHECK_INT(C.motion_cal_src, MOTION_CAL_NONE);
    runtime_cal_post(runtime_dev_gen(), &cal, MCAL_OK, 0);
    CHECK(runtime_cal_drain(&C));
    CHECK_INT(C.motion_cal_src, MOTION_CAL_FACTORY);
    CHECK(!runtime_cal_drain(&C));                         /* taken once                  */

    /* the snapshot carries it all to the mapper */
    static dsxp_snap_t snap;
    motion_fresh(false, 20);
    feed_m(&C, 128, 0, 0, 0x08, 0, 20, 1.02);
    core_tick(&C, 1.02);
    core_snapshot(&C, 1.02, &snap);
    CHECK(snap.motion.enabled && snap.motion_seeded && snap.sent_valid);
    core_snapshot(&C, 1.02 + 5.0, &snap);                  /* reports stopped, no removal */
    CHECK(!snap.motion_seeded);
    CHECK_NEAR(snap.motion_angle[MOTION_ROLL], 20, 0.01);
    CHECK_NEAR(snap.motion_out[MOTION_ROLL], want, 1e-4);
    CHECK_NEAR(snap.sent[MOTION_ROLL], want, 1e-4);
}

void test_core(void) {
    runtime_smoke();
    guard_packets();
    gate_and_overrides();
    staleness();
    button_gaps();
    brakes_and_releases();
    telemetry_and_triggers();
    light_sim_alone();
    saves_and_output();
    suspend_and_dry_run();
    throttle();
    xp12_telemetry();
    motion_control();

    CHECK(log_name_is_ours("reports-2026-01-02.log"));
    CHECK(log_name_is_ours("events-2026-11-05.log"));
    CHECK(!log_name_is_ours("reports-2026-1-02.log"));
    CHECK(!log_name_is_ours("bindings.conf"));
    CHECK(!log_name_is_ours("events-2026-11-05.log.bak"));
}
