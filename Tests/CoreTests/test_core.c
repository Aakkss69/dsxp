/* Core behaviour with a fake clock and captured packets: the gate, the override
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
    CHECK_INT(drefs(DR_ROLL, NULL), 1);              /* no more roll values after the centre */
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
    t.u.trig.v = (trig_manual_t){ TFX_RESIST, 30, 0, 128, 40 };
    t.u.trig.mask = TF_MODE | TF_START | TF_END | TF_STRENGTH;
    core_apply_cmd(&C, &t, 1.0);
    CHECK_INT(C.fx_out[0].strength, 128);
    cmd_t w = { .kind = CMD_FXMODE }; w.u.mode = FXM_WIND;
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
    w.u.mode = FXM_MANUAL;
    core_apply_cmd(&C, &w, 1.2);
    CHECK_INT(C.fx_out[0].strength, 128);

    /* telemetry stops: everything sim-driven is released */
    cmd_t rm = { .kind = CMD_RUMBLEMODE }; rm.u.mode = RM_SIM;
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
    cf.s.fxmode = FXM_WIND;
    cf.bridge.telemetry_hz = 20;
    core_apply_config(&C, &cf, CFB_CONTROLS | CFB_FEEDBACK, 21.6);
    CHECK_NEAR(C.s.tune.curve, 2.0, 1e-12);
    CHECK_INT(C.s.fxmode, FXM_WIND);
    CHECK(!strcmp(C.wx.note, "waiting for X-Plane"));
    CHECK_NEAR(C.bridge.telemetry_hz, 10, 1e-12);     /* bridge.conf was not in the mask */
    CHECK_INT(core_save_due(&C, 30.0), 0);
    cf.s.fxmode = FXM_MANUAL;
    core_apply_config(&C, &cf, CFB_FEEDBACK | CFB_BRIDGE, 21.7);
    CHECK_INT(C.s.fxmode, FXM_MANUAL);
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
    C.s.rmode = RM_SIM; C.s.fxmode = FXM_WIND;
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
    CHECK(!strcmp(C.rnote, "paused"));
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

/* What the guard sends after a crash: centred yoke, brakes off, all four
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

void test_core(void) {
    runtime_smoke();
    guard_packets();
    gate_and_overrides();
    staleness();
    telemetry_and_triggers();
    light_sim_alone();
    saves_and_output();
    suspend_and_dry_run();
    throttle();
    xp12_telemetry();

    CHECK(log_name_is_ours("reports-2026-01-02.log"));
    CHECK(log_name_is_ours("events-2026-11-05.log"));
    CHECK(!log_name_is_ours("reports-2026-1-02.log"));
    CHECK(!log_name_is_ours("bindings.conf"));
    CHECK(!log_name_is_ours("events-2026-11-05.log.bak"));
}
