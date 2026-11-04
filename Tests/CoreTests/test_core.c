/* Core behaviour with a fake clock and captured packets: the gate, the override
 * planner, input staleness, telemetry, wind vs manual triggers, saves, output. */
#include "tests.h"
#include <string.h>

typedef struct { unsigned char d[520]; size_t n; } pkt_t;
static pkt_t P[8192];
static int NP, OUTN, SAVES;
static unsigned char OUTLAST[OUT_REPORT_LEN];
static char SAVED[16384];
static size_t SAVEDN;

static void cap_udp(void *ctx, const unsigned char *b, size_t n) {
    (void)ctx;
    if (NP < 8192) { memcpy(P[NP].d, b, n < 520 ? n : 520); P[NP].n = n; NP++; }
}
static void cap_out(void *ctx, const unsigned char *r) { (void)ctx; OUTN++; memcpy(OUTLAST, r, OUT_REPORT_LEN); }
static void cap_save(void *ctx, const char *t, size_t n) {
    (void)ctx; SAVES++;
    SAVEDN = n < sizeof SAVED - 1 ? n : sizeof SAVED - 1;
    memcpy(SAVED, t, SAVEDN); SAVED[SAVEDN] = 0;
}
static void reset_cap(void) { NP = OUTN = SAVES = 0; }

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

static const core_sinks_t SK = { NULL, cap_udp, cap_out, cap_save };
#define OV_ROLL  "sim/operation/override/override_joystick_roll"
#define OV_PITCH "sim/operation/override/override_joystick_pitch"
#define OV_HDG   "sim/operation/override/override_joystick_heading"
#define OV_BRK   "sim/operation/override/override_toe_brakes"
#define DR_ROLL  "sim/joystick/yoke_roll_ratio"

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
    CHECK_INT(drefs(DR_ROLL, NULL), 1);              /* no more roll values after the centre */
    CHECK_INT(drefs_with(OV_ROLL, 1.0f), 0);

    /* USB plugged in: X-Plane's native path wins, overrides released twice */
    reset_cap();
    core_set_gate(&C, true, true, true, "X-Plane", 7.0);
    CHECK(!C.engaged);
    CHECK_INT(drefs_with(OV_PITCH, 0.0f), 2);
}

static void staleness(void) {
    fresh();
    feed(&C, 128, 0x08, 10.0);
    core_set_gate(&C, true, false, true, "X-Plane", 10.0);
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
    CHECK(!C.prev_btn[B_CROSS]);
    core_tick(&C, 10.0 + INPUT_LOST_SEC + 0.1);
    CHECK(C.force_gate);                            /* gate re-runs immediately */
    core_set_gate(&C, true, false, true, "X-Plane", 10.0 + INPUT_LOST_SEC + 0.1);
    CHECK(!C.engaged);
}

static void telemetry_and_triggers(void) {
    fresh();
    /* manual Resist 128 on L2, then wind mode */
    cmd_t t = { .kind = CMD_TRIGGER, .key = 0 };
    t.u.trig.v = (trigger_fx_t){ TFX_RESIST, 30, 0, 128 };
    t.u.trig.mask = TF_MODE | TF_START | TF_END | TF_STRENGTH;
    core_apply_cmd(&C, &t, 1.0);
    CHECK_INT(C.fx_out[0].strength, 128);
    cmd_t w = { .kind = CMD_FXMODE }; w.u.mode = FXM_WIND;
    core_apply_cmd(&C, &w, 1.0);

    core_set_gate(&C, false, false, true, "X-Plane", 1.0);   /* sim up, no controller */
    core_tick(&C, 1.0);
    CHECK_INT(rrefs(TELEMETRY_HZ), 14);              /* all 14 datarefs subscribed */

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
    char buf[16384];
    bindings_format(&C.s, buf, sizeof buf);
    CHECK(strstr(buf, "trigger l 1 30 0 128\n") != NULL);
    w.u.mode = FXM_MANUAL;
    core_apply_cmd(&C, &w, 1.2);
    CHECK_INT(C.fx_out[0].strength, 128);

    /* telemetry stops: everything sim-driven is released */
    cmd_t rm = { .kind = CMD_RUMBLEMODE }; rm.u.mode = RM_SIM;
    core_apply_cmd(&C, &rm, 1.2);
    int32_t ids2[] = { 7, 9, 10, 11 };
    float vals2[] = { 1.0f, 1.0f, 1.0f, 1.0f };       /* crashed, left wing stalled */
    udp_pairs(&C, ids2, vals2, 4, true, 1.3);
    core_tick(&C, 1.3);
    CHECK_INT(C.rumble.left, 255);
    CHECK_NEAR(C.wx.stall_l, 1.0, 1e-6);
    core_tick(&C, 1.3 + TELEMETRY_TIMEOUT_SEC + 0.1);
    CHECK(!C.wx.live);
    CHECK_INT(C.rumble.left, 0);
    CHECK(!C.wx.crashed);
    CHECK_NEAR(C.sl[0] + C.sl[1] + C.sl[2], 0.0, 0);
    CHECK(!strcmp(C.wx.note, "telemetry lost"));
}

static void light_sim_alone(void) {
    fresh();
    C.s.light.mode = LM_SIM;                          /* only the light bar is sim-driven */
    core_set_gate(&C, false, false, true, "X-Plane", 1.0);
    core_tick(&C, 1.0);
    CHECK(rrefs(TELEMETRY_HZ) > 0);
    int32_t ids[] = { 1 };
    float vals[] = { 80.0f };
    udp_pairs(&C, ids, vals, 1, true, 1.1);
    core_tick(&C, 1.1);
    CHECK(!strcmp(C.lnote, "clean"));

    /* X-Plane quits: unsubscribed (frequency 0) */
    reset_cap();
    core_set_gate(&C, false, false, false, "", 2.0);
    core_tick(&C, 2.0);
    CHECK_INT(rrefs(0), 14);
}

static void saves_and_output(void) {
    fresh();
    cmd_t tn = { .kind = CMD_TUNE };
    tn.u.tune.has_dz = true; tn.u.tune.dz = 0.2;
    core_apply_cmd(&C, &tn, 20.0);
    core_tick(&C, 20.4);
    CHECK_INT(SAVES, 0);                             /* debounced */
    core_tick(&C, 20.6);
    CHECK_INT(SAVES, 1);
    settings_t s;
    load_defaults(&s);
    bindings_parse(&s, SAVED, SAVEDN);
    CHECK_NEAR(s.tune.deadzone, 0.2, 1e-4);

    /* output reports go out only with a device, then quiet on demand */
    core_tick(&C, 21.0);
    CHECK_INT(OUTN, 0);
    core_on_device(&C, true, true);
    core_tick(&C, 21.1);
    CHECK(OUTN >= 1);
    out_quiet(&C);
    CHECK_INT(OUTLAST[3], 0x0F);
    CHECK_INT(OUTLAST[5], 0); CHECK_INT(OUTLAST[6], 0);
    CHECK_INT(OUTLAST[47], 0);
    core_on_device(&C, true, false);
    CHECK(!C.have_report);
}

static void suspend_and_dry_run(void) {
    fresh();
    feed(&C, 128, 0x08, 1.0);
    core_set_gate(&C, true, false, true, "X-Plane", 1.0);
    CHECK(C.engaged);
    cmd_t sp = { .kind = CMD_SUSPEND }; sp.u.flag = true;
    core_apply_cmd(&C, &sp, 1.1);
    CHECK(!C.engaged);
    core_set_gate(&C, true, false, true, "X-Plane", 1.1);
    CHECK(!C.engaged);
    sp.u.flag = false;
    core_apply_cmd(&C, &sp, 1.2);
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

void test_core(void) {
    gate_and_overrides();
    staleness();
    telemetry_and_triggers();
    light_sim_alone();
    saves_and_output();
    suspend_and_dry_run();

    CHECK(log_name_is_ours("reports-2026-01-02.log"));
    CHECK(log_name_is_ours("events-2026-11-05.log"));
    CHECK(!log_name_is_ours("reports-2026-1-02.log"));
    CHECK(!log_name_is_ours("bindings.conf"));
    CHECK(!log_name_is_ours("events-2026-11-05.log.bak"));
}
