/* The config files: schema, parser, writer, merge, cross-key rules, the
 * committed configs/ folder, odd and hostile input, locale, fuzzing. */
#include "tests.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>

static config_t A, B, D;              /* too big for the stack */
static char T1[64 * 1024], T2[64 * 1024];

static size_t fmt(conf_file_t f, const config_t *c, char *buf) {
    size_t n = conf_format(f, c, buf, sizeof T1);
    CHECK(n > 0 && n < sizeof T1);
    return n;
}

/* parse text over the defaults; returns the warning count */
static int parse(conf_file_t f, const char *text, config_t *c, conf_warns_t *w) {
    config_defaults(c);
    memset(w, 0, sizeof *w);
    conf_parse(f, text, strlen(text), c, w);
    return w->n;
}

static bool has_warn(const conf_warns_t *w, const char *key, int line) {
    for (int i = 0; i < w->n; i++)
        if (!strcmp(w->w[i].key, key) && (line < 0 || w->w[i].line == line)) return true;
    return false;
}

static void schema_sanity(void) {
    config_defaults(&D);
    const char *seen[400];
    int nseen = 0;
    for (int f = 0; f < CF_COUNT; f++) {
        const conf_schema_t *sc = conf_schema((conf_file_t)f);
        CHECK(sc && sc->file && sc->intro && sc->nkeys > 0 && sc->nkeys < 256);
        CHECK_INT(sc->rewritten, f != CF_BRIDGE);
        for (int i = 0; i < sc->nkeys; i++) {
            const conf_key_t *k = &sc->keys[i];
            if (k->type == CT_SECTION) continue;
            CHECK(k->off < sizeof(config_t));
            if (k->type != CT_BUTTON && k->type != CT_AXIS) CHECK(k->doc && *k->doc);
            CHECK(strlen(k->key) < sizeof ((conf_warn_t *)0)->key);
            for (int j = 0; j < nseen; j++) if (!strcasecmp(seen[j], k->key)) CHECK(!"duplicate key");
            if (nseen < 400) seen[nseen++] = k->key;
            CHECK(conf_find((conf_file_t)f, k->key) == k);
            if (k->type == CT_ENUM || k->type == CT_ENUMB) CHECK(k->names && k->nnames >= 2);
            if (k->type == CT_INT || k->type == CT_U8 || k->type == CT_REAL || k->type == CT_PCT ||
                k->type == CT_PCTB || k->type == CT_TIME) {
                CHECK(k->lo < k->hi);
                CHECK(k->unit != NULL);
                /* the default is inside the range it documents */
                char v[64];
                conf_format_value(k, &D, v, sizeof v);
                double d = strtod_l(v, NULL, NULL);
                if (!(d >= k->lo && d <= k->hi)) {
                    fprintf(stderr, "default of %s (%s) is outside %g..%g\n", k->key, v, k->lo, k->hi);
                    CHECK(!"default out of range");
                }
            }
        }
    }
    CHECK_INT(nseen, 18 + 9 + 6 + 18 + 33 + 1 + 10 + 13 + 6 + 11);   /* motion: 12 + 6 calibration */
    CHECK(conf_find(CF_CONTROLS, "STICK.DEADZONE") != NULL);      /* keys are case-insensitive */
    CHECK(conf_find(CF_BRIDGE, "stick.deadzone") == NULL);
}

static void round_trips(void) {
    conf_warns_t w;
    /* defaults: format, parse, format again gives identical text and no warnings */
    config_defaults(&D);
    for (int f = 0; f < CF_COUNT; f++) {
        size_t n = fmt((conf_file_t)f, &D, T1);
        memset(&A, 0x5A, sizeof A);                  /* garbage everywhere */
        memset(&w, 0, sizeof w);
        conf_parse((conf_file_t)f, T1, n, &A, &w);
        CHECK_INT(w.n, 0);
        size_t m = fmt((conf_file_t)f, &A, T2);
        CHECK(m == n && !memcmp(T1, T2, n));
    }

    /* a changed configuration, every kind of key */
    config_defaults(&A);
    A.bridge.port = 49001; A.bridge.input_stale_s = 0.4; A.bridge.log_verbose = true;
    A.s.tune.deadzone = 0.123; A.s.gesture.tap_time_s = 0.2;
    A.s.axis[S_RY] = (axis_map_t){ AX_THROTTLE, true };
    snprintf(A.s.bind[B_TP_SWIPE_UL].cmd, CMD_PATH_MAX, "sim/view/forward_with_2d_panel");
    A.s.bind[B_TP_SWIPE_UL].repeat = true;
    A.s.bind[B_DP_R].cmd[0] = 0;
    A.s.fx_manual[1] = (trig_manual_t){ TFX_VIBRATE, 20, 200, 153, 33 };
    A.s.trigger_source = TRIGGER_SRC_WIND; A.s.rumble_source = RUMBLE_SRC_SIM; A.s.rumble_v2 = true;
    A.s.light = (light_t){ 0x12, 0xAB, 0xEF, false, LP_RAINBOW, 0.25, 0.5, LIGHT_SRC_SIM, 0x11 };
    A.s.wind.full_force_kt = 250; A.s.rumble.crash_s = 0;
    for (int f = 0; f < CF_COUNT; f++) {
        size_t n = fmt((conf_file_t)f, &A, T1);
        config_defaults(&B);
        memset(&w, 0, sizeof w);
        conf_parse((conf_file_t)f, T1, n, &B, &w);
        CHECK_INT(w.n, 0);
        size_t m = fmt((conf_file_t)f, &B, T2);
        CHECK(m == n && !memcmp(T1, T2, n));
    }
    config_defaults(&B);
    for (int f = 0; f < CF_COUNT; f++) {
        size_t n = fmt((conf_file_t)f, &A, T1);
        conf_parse((conf_file_t)f, T1, n, &B, &w);
    }
    CHECK_INT(B.bridge.port, 49001); CHECK_NEAR(B.bridge.input_stale_s, 0.4, 1e-9);
    CHECK(B.bridge.log_verbose);
    CHECK_NEAR(B.s.tune.deadzone, 0.123, 1e-9);
    CHECK_INT(B.s.axis[S_RY].target, AX_THROTTLE); CHECK(B.s.axis[S_RY].invert);
    CHECK(!strcmp(B.s.bind[B_TP_SWIPE_UL].cmd, "sim/view/forward_with_2d_panel"));
    CHECK(B.s.bind[B_TP_SWIPE_UL].repeat); CHECK(!B.s.bind[B_DP_R].cmd[0]);
    CHECK_INT(B.s.fx_manual[1].mode, TFX_VIBRATE); CHECK_INT(B.s.fx_manual[1].freq, 33);
    CHECK_INT(B.s.fx_manual[1].strength, 153);       /* 60.0 % of 255 */
    CHECK_INT(B.s.light.r, 0x12); CHECK_INT(B.s.light.g, 0xAB); CHECK_INT(B.s.light.b, 0xEF);
    CHECK_INT(B.s.light.player, 0x11); CHECK_INT(B.s.light.source, LIGHT_SRC_SIM);
    CHECK_INT(B.s.light.pattern, LP_RAINBOW); CHECK(!B.s.light.on);
    CHECK_NEAR(B.s.wind.full_force_kt, 250, 1e-9); CHECK_NEAR(B.s.rumble.crash_s, 0, 1e-12);

    /* the text explains itself: doc, range line, value line */
    config_defaults(&D);
    fmt(CF_CONTROLS, &D, T1);
    CHECK(strstr(T1, "#   Range 0.0 to 45.0 %   Default 6.0 %\nstick.deadzone = 6.0 %\n") != NULL);
    CHECK(strstr(T1, "button.dpad_right = sim/flight_controls/landing_gear_down once\n") != NULL);
    fmt(CF_FEEDBACK, &D, T1);
    CHECK(strstr(T1, "light.color = #005AFF\n") != NULL);
    CHECK(strstr(T1, "#   One of: off, resist, weapon, vibrate") != NULL);
    fmt(CF_BRIDGE, &D, T1);
    CHECK(strstr(T1, "xplane.port = 49000\n") != NULL);
    CHECK(strstr(T1, "never writes it again") != NULL);
    CHECK(strstr(T1, "timing.input_stale = 250 ms\n") != NULL);
}

/* The configs/ folder in the repository must be exactly what the code writes. */
static void committed_files(void) {
    char root[1024];
    snprintf(root, sizeof root, "%s", __FILE__);
    char *cut = strstr(root, "Tests/CoreTests/test_config.c");
    if (!cut) { CHECK(!"cannot locate the repository from __FILE__"); return; }
    *cut = 0;
    config_defaults(&D);
    for (int f = 0; f < CF_COUNT; f++) {
        char path[1200];
        snprintf(path, sizeof path, "%sconfigs/%s", root, conf_schema((conf_file_t)f)->file);
        size_t len = 0;
        char *text = file_read_all(path, CONF_MAX_BYTES, &len);
        size_t n = fmt((conf_file_t)f, &D, T1);
        if (!text || len != n || memcmp(text, T1, n) != 0) {
            fprintf(stderr, "%s differs from the generated defaults: run Scripts/test-core.sh "
                            "--update-configs\n", path);
            CHECK(!"configs/ drifted");
        }
        free(text);
    }
}

static void odd_input(void) {
    conf_warns_t w;
    /* CRLF, BOM, spacing, case, inline comments, units optional */
    parse(CF_CONTROLS, "\xEF\xBB\xBFStick.Deadzone=12.5%\r\n  stick.curve   =  2.00  # steeper\r\n"
                       "gesture.tap_time = 0.25 s\r\n", &A, &w);
    CHECK_INT(w.n, 0);
    CHECK_NEAR(A.s.tune.deadzone, 0.125, 1e-12);
    CHECK_NEAR(A.s.tune.curve, 2.0, 1e-12);
    CHECK_NEAR(A.s.gesture.tap_time_s, 0.25, 1e-12);
    parse(CF_BRIDGE, "timing.input_stale = 0.3 s\ntiming.input_lost = 1500 ms\ntelemetry.rate = 20\n", &A, &w);
    CHECK_INT(w.n, 0);
    CHECK_NEAR(A.bridge.input_stale_s, 0.3, 1e-12);
    CHECK_NEAR(A.bridge.input_lost_s, 1.5, 1e-12);
    CHECK_NEAR(A.bridge.telemetry_hz, 20, 1e-12);

    /* a missing key means its default, whatever the value was before */
    config_defaults(&A);
    A.s.tune.curve = 2.9;
    memset(&w, 0, sizeof w);
    conf_parse(CF_CONTROLS, "stick.deadzone = 10 %\n", 21, &A, &w);
    CHECK_NEAR(A.s.tune.curve, 1.0, 1e-12);
    CHECK_NEAR(A.s.tune.deadzone, 0.10, 1e-12);

    /* unusable numbers keep the previous value, with a warning on that line */
    config_defaults(&A);
    A.s.tune.deadzone = 0.2;
    A.s.tune.curve = 2.0;
    A.s.gesture.tap_time_s = 0.3;
    memset(&w, 0, sizeof w);
    const char *bad = "stick.deadzone = nan %\nstick.curve = 1e999\ngesture.tap_time = inf ms\n"
                      "gesture.pulse = 12abc\ngesture.sensitivity = 50 kt\n";
    conf_parse(CF_CONTROLS, bad, strlen(bad), &A, &w);
    CHECK_NEAR(A.s.tune.deadzone, 0.2, 1e-12);
    CHECK_NEAR(A.s.tune.curve, 2.0, 1e-12);
    CHECK_NEAR(A.s.gesture.tap_time_s, 0.3, 1e-12);
    CHECK_NEAR(A.s.gesture.pulse_s, GESTURE_DEFAULTS.pulse_s, 1e-12);     /* "12abc": bad unit */
    CHECK_NEAR(A.s.tune.gesture, 0.5, 1e-12);
    CHECK(has_warn(&w, "stick.deadzone", 1)); CHECK(has_warn(&w, "stick.curve", 2));
    CHECK(has_warn(&w, "gesture.tap_time", 3)); CHECK(has_warn(&w, "gesture.pulse", 4));
    CHECK(has_warn(&w, "gesture.sensitivity", 5));

    /* out of range: clamped, with a warning */
    parse(CF_CONTROLS, "stick.deadzone = 90 %\nstick.curve = 0.1\n", &A, &w);
    CHECK_NEAR(A.s.tune.deadzone, 0.45, 1e-12);
    CHECK_NEAR(A.s.tune.curve, 0.4, 1e-12);
    CHECK_INT(w.n, 2);
    parse(CF_BRIDGE, "xplane.port = 80\n", &A, &w);
    CHECK_INT(A.bridge.port, 1024);
    CHECK(has_warn(&w, "xplane.port", 1));

    /* duplicates: last wins; unknown keys and keys from another file: ignored */
    parse(CF_CONTROLS, "stick.curve = 1.5\nstick.curve = 2.5\nstick.wobble = 3\nlight.on = no\nnonsense\n", &A, &w);
    CHECK_NEAR(A.s.tune.curve, 2.5, 1e-12);
    CHECK(has_warn(&w, "stick.curve", 2));
    CHECK(has_warn(&w, "stick.wobble", 3));
    CHECK(has_warn(&w, "light.on", 4));
    CHECK(strstr(w.w[2].msg, "feedback.conf") != NULL);
    CHECK(has_warn(&w, "", 5));
    CHECK(A.s.light.on);                             /* not applied from the wrong file */
    /* a key DSXP once wrote and no longer uses: read without a warning */
    CHECK_INT(parse(CF_CONTROLS, "motion.yaw_return = 4.0 s\nmotion.yaw.full = 25 deg\n", &A, &w), 0);
    CHECK_NEAR(A.s.motion.full_deg[MOTION_YAW], 25, 1e-12);

    /* enums, booleans, colors, lamps */
    parse(CF_FEEDBACK, "trigger.source = WIND\nlight.on = Off\nlight.color = #12ab9F   # teal\n"
                       "light.player_lamps = x.x.X\nrumble.path = v2\nlight.pattern = Breathe\n", &A, &w);
    CHECK_INT(w.n, 0);
    CHECK_INT(A.s.trigger_source, TRIGGER_SRC_WIND); CHECK(!A.s.light.on);
    CHECK_INT(A.s.light.r, 0x12); CHECK_INT(A.s.light.g, 0xAB); CHECK_INT(A.s.light.b, 0x9F);
    CHECK_INT(A.s.light.player, 0x15); CHECK(A.s.rumble_v2); CHECK_INT(A.s.light.pattern, LP_BREATHE);
    parse(CF_FEEDBACK, "light.player_lamps = three\nlight.color = #12ab9\nlight.on = maybe\n"
                       "trigger.left.mode = punch\n", &A, &w);
    CHECK_INT(A.s.light.player, 0x0E);
    CHECK_INT(A.s.light.r, 0x00); CHECK_INT(A.s.light.b, 0xFF);   /* bad values keep the previous one */
    CHECK(A.s.light.on);
    CHECK_INT(A.s.fx_manual[0].mode, TFX_OFF);
    CHECK_INT(w.n, 3);
    conf_format_value(conf_find(CF_FEEDBACK, "light.player_lamps"), &A, T1, 64);
    CHECK(!strcmp(T1, "three"));
    parse(CF_FEEDBACK, "light.player_lamps = x.y..\n", &A, &w);
    CHECK_INT(A.s.light.player, 0x04);
    CHECK_INT(w.n, 1);
    A.s.light.player = 0x13;
    conf_format_value(conf_find(CF_FEEDBACK, "light.player_lamps"), &A, T1, 64);
    CHECK(!strcmp(T1, "xx..x"));

    /* buttons and axes */
    parse(CF_CONTROLS, "button.cross = sim/flight_controls/flaps_down hold\nbutton.circle = none\n"
                       "button.square = sim/a b c\nbutton.triangle = sim/x\"y\nbutton.l1 = sim/ok once\n"
                       "button.r1 = sim/ok sometimes\naxis.left_x = pitch inverted\naxis.left_y = warp\n"
                       "axis.right_x = YAW normal\n", &A, &w);
    CHECK(!strcmp(A.s.bind[B_CROSS].cmd, "sim/flight_controls/flaps_down")); CHECK(A.s.bind[B_CROSS].repeat);
    CHECK(!A.s.bind[B_CIRCLE].cmd[0]);
    CHECK(!A.s.bind[B_L1].repeat); CHECK(!strcmp(A.s.bind[B_L1].cmd, "sim/ok"));
    CHECK(has_warn(&w, "button.square", 3)); CHECK(has_warn(&w, "button.triangle", 4));
    CHECK(has_warn(&w, "button.r1", 6));
    CHECK_INT(A.s.axis[S_LX].target, AX_PITCH); CHECK(A.s.axis[S_LX].invert);
    CHECK_INT(A.s.axis[S_LY].target, AX_PITCH);      /* default kept */
    CHECK(has_warn(&w, "axis.left_y", 8));
    CHECK_INT(A.s.axis[S_RX].target, AX_YAW); CHECK(!A.s.axis[S_RX].invert);

    /* NUL bytes, overlong lines, a value that starts with # */
    char nul[] = "stick.curve = 2.0\0junk\nstick.deadzone = 9 %\n";
    config_defaults(&A);
    memset(&w, 0, sizeof w);
    conf_parse(CF_CONTROLS, nul, sizeof nul - 1, &A, &w);
    CHECK_NEAR(A.s.tune.curve, 1.0, 1e-12);
    CHECK_NEAR(A.s.tune.deadzone, 0.09, 1e-12);
    CHECK(has_warn(&w, "", 1));
    char *big = malloc(3000);
    memset(big, ' ', 2000);
    memcpy(big, "stick.curve = 2.0", 17);
    memcpy(big + 2000, "\nstick.deadzone = 9 %\n", 23);
    parse(CF_CONTROLS, big, &A, &w);
    CHECK_NEAR(A.s.tune.curve, 1.0, 1e-12);
    CHECK_NEAR(A.s.tune.deadzone, 0.09, 1e-12);
    free(big);
    parse(CF_FEEDBACK, "light.color = # nothing\n", &A, &w);
    CHECK_INT(w.n, 1);

    /* too many warnings: counted, not overflowed */
    char many[8192] = "";
    for (int i = 0; i < 60; i++) strcat(many, "x.y = 1\n");
    parse(CF_BRIDGE, many, &A, &w);
    CHECK_INT(w.n, CONF_WARN_MAX);
    CHECK_INT(w.dropped, 60 - CONF_WARN_MAX);
}

static void merge_and_rules(void) {
    /* the mapper changed the curve; someone edited the deadzone in the file */
    static config_t base, ours, theirs, out;
    config_defaults(&base);
    ours = base; theirs = base;
    ours.s.tune.curve = 2.0;
    theirs.s.tune.deadzone = 0.2;
    theirs.s.tune.curve = base.s.tune.curve;
    ours.s.light.on = false;                          /* other file: not part of this merge */
    theirs.s.light.on = true;
    conf_merge(CF_CONTROLS, &base, &ours, &theirs, &out);
    CHECK_NEAR(out.s.tune.curve, 2.0, 1e-12);
    CHECK_NEAR(out.s.tune.deadzone, 0.2, 1e-12);
    CHECK(!out.s.light.on);
    /* both changed the same key: the mapper's value wins */
    theirs.s.tune.curve = 2.5;
    conf_merge(CF_CONTROLS, &base, &ours, &theirs, &ours);   /* in place */
    CHECK_NEAR(ours.s.tune.curve, 2.0, 1e-12);
    CHECK_NEAR(ours.s.tune.deadzone, 0.2, 1e-12);

    /* each cross-key rule corrects the later key and says so in the right file */
    conf_warns_t w[CF_COUNT];
    config_defaults(&A);
    memset(w, 0, sizeof w);
    config_validate(&A, w);
    CHECK_INT(w[0].n + w[1].n + w[2].n, 0);          /* the defaults obey every rule */
    A.bridge.telemetry_hz = 2; A.bridge.telemetry_timeout_s = 1.0;
    A.bridge.tick_hz = 30; A.s.gesture.pulse_s = 0.02;
    A.s.gesture.swipe_length_at0 = 0.05; A.s.gesture.tap_wander_at0 = 0.07;
    A.s.gesture.swipe_length_at100 = 0.04; A.s.gesture.tap_wander_at100 = 0.04;
    A.s.rumble.jolt_start_g = 4; A.s.rumble.jolt_full_g = 3;
    A.s.wind.buffet_strength_min = 0.5; A.s.wind.buffet_strength_max = 0.4;
    A.s.wind.buffet_freq_min_hz = 30; A.s.wind.buffet_freq_max_hz = 20;
    A.s.light_sim.stall_alarm = 0.5; A.s.light_sim.stall_caution = 0.5;
    config_validate(&A, w);
    CHECK_INT(w[CF_BRIDGE].n, 1); CHECK_INT(w[CF_CONTROLS].n, 3); CHECK_INT(w[CF_FEEDBACK].n, 4);
    CHECK_NEAR(A.bridge.telemetry_timeout_s, 1.5, 1e-12);
    CHECK_NEAR(A.s.gesture.pulse_s, 0.05, 1e-12);
    CHECK(A.s.gesture.swipe_length_at0 > A.s.gesture.tap_wander_at0);
    CHECK(A.s.gesture.swipe_length_at100 > A.s.gesture.tap_wander_at100);
    CHECK_NEAR(A.s.rumble.jolt_full_g, 4.5, 1e-12);
    CHECK_NEAR(A.s.wind.buffet_strength_max, 0.5, 1e-12);
    CHECK_NEAR(A.s.wind.buffet_freq_max_hz, 30, 1e-12);
    CHECK(A.s.light_sim.stall_caution < A.s.light_sim.stall_alarm);
    CHECK(has_warn(&w[CF_FEEDBACK], "rumble.g_jolt_full", 0));

    /* motion: a full-deflection angle at or inside its deadzone is pushed out to
     * deadzone + 1 deg (yaw against its own deadzone), in whole degrees */
    const char *mtext = "motion.deadzone = 10\nmotion.roll.full = 10\n"
                        "motion.yaw.deadzone = 12.5\nmotion.yaw.full = 5\n";
    CHECK_INT(parse(CF_CONTROLS, mtext, &A, &w[CF_CONTROLS]), 0);   /* each value in range */
    memset(w, 0, sizeof w);
    config_validate(&A, w);
    CHECK_INT(w[CF_CONTROLS].n, 2);
    CHECK(has_warn(&w[CF_CONTROLS], "motion.roll.full", 0));
    CHECK(has_warn(&w[CF_CONTROLS], "motion.yaw.full", 0));
    CHECK_NEAR(A.s.motion.full_deg[MOTION_ROLL], 11, 1e-12);
    CHECK_NEAR(A.s.motion.full_deg[MOTION_YAW], 14, 1e-12);
    CHECK_NEAR(A.s.motion.full_deg[MOTION_PITCH], 30, 1e-12);       /* untouched */
    /* the source words read back into the flags, either case */
    CHECK_INT(parse(CF_CONTROLS, "motion.enabled = yes\nmotion.yaw = MOTION\nmotion.roll = sticks\n",
                    &A, &w[CF_CONTROLS]), 0);
    CHECK(A.s.motion.enabled && A.s.motion.use[MOTION_YAW] && !A.s.motion.use[MOTION_ROLL]);
    memset(w, 0, sizeof w);
    config_validate(&A, w);                                          /* not calibrated:    */
    CHECK(has_warn(&w[CF_CONTROLS], "motion.enabled", 0));           /* switched off, said */
    CHECK(!A.s.motion.enabled);
    CHECK_INT(parse(CF_CONTROLS, "motion.enabled = yes\nmotion.calibrated = yes\nmotion.level.pitch = 12.5\n"
                    "motion.gyro_bias.y = -0.801\n", &A, &w[CF_CONTROLS]), 0);
    memset(w, 0, sizeof w);
    config_validate(&A, w);
    CHECK(A.s.motion.enabled && A.s.motion.calibrated);
    CHECK_NEAR(A.s.motion.level_pitch_deg, 12.5, 1e-12);
    CHECK_NEAR(A.s.motion.gyro_bias_dps[1], -0.801, 1e-12);
    CHECK_INT(parse(CF_CONTROLS, "motion.gyro_bias.x = 25\n", &A, &w[CF_CONTROLS]), 1);   /* out of range */
    CHECK(A.s.motion.use[MOTION_PITCH]);                              /* default kept */
    CHECK_INT(parse(CF_CONTROLS, "motion.roll = tilt\n", &A, &w[CF_CONTROLS]), 1);
    CHECK(A.s.motion.use[MOTION_ROLL]);                               /* bad word: previous value */
}

/* A German locale writes 6,0 with printf: the files must not change with it. */
static void locale_proof(void) {
    const char *names[] = { "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8" };
    char saved[256];
    const char *cur = setlocale(LC_ALL, NULL);
    snprintf(saved, sizeof saved, "%s", cur ? cur : "C");
    const char *got = NULL;
    for (int i = 0; i < 3 && !got; i++) got = setlocale(LC_ALL, names[i]);
    if (!got) { fprintf(stderr, "note: no comma-decimal locale installed; locale test skipped\n"); return; }
    char probe[16];
    snprintf(probe, sizeof probe, "%.1f", 6.0);
    CHECK(!strcmp(probe, "6,0"));                    /* the locale really is active */
    config_defaults(&D);
    fmt(CF_CONTROLS, &D, T1);
    CHECK(strstr(T1, "stick.deadzone = 6.0 %\n") != NULL);
    conf_warns_t w;
    parse(CF_CONTROLS, "stick.deadzone = 12.5 %\nstick.curve = 2,5\n", &A, &w);
    CHECK_NEAR(A.s.tune.deadzone, 0.125, 1e-12);
    CHECK_NEAR(A.s.tune.curve, 1.0, 1e-12);          /* "2,5" is not a number here */
    CHECK_INT(w.n, 1);
    setlocale(LC_ALL, saved);
}

/* Random byte changes to real files: whatever comes out must be a valid config
 * (writing it and reading it back gives no warnings and the same text). */
static void fuzz(void) {
    static char m[64 * 1024];
    config_defaults(&D);
    uint32_t x = 88172645u;
    int bad = 0;
    for (int it = 0; it < 30000 && bad < 3; it++) {
        conf_file_t f = (conf_file_t)(it % CF_COUNT);
        size_t n = conf_format(f, &D, T1, sizeof T1);
        memcpy(m, T1, n);
        int k = 1 + (int)(x % 12);
        for (int j = 0; j < k; j++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            size_t pos = x % n;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            m[pos] = (x & 0x100) ? (char)(x & 0xFF) : "0123456789.-#=x %\n "[x % 19];
        }
        conf_warns_t w;
        memset(&w, 0, sizeof w);
        config_defaults(&A);
        conf_parse(f, m, n, &A, &w);
        size_t n1 = conf_format(f, &A, T1, sizeof T1);
        memset(&w, 0, sizeof w);
        config_defaults(&B);
        conf_parse(f, T1, n1, &B, &w);
        size_t n2 = conf_format(f, &B, T2, sizeof T2);
        if (w.n != 0 || n1 == 0 || n1 != n2 || memcmp(T1, T2, n1) != 0) {
            bad++;
            fprintf(stderr, "fuzz %d (%s): re-read gave %d warnings%s%s\n", it, conf_schema(f)->file, w.n,
                    w.n ? ", first: " : "", w.n ? w.w[0].msg : "");
        }
    }
    CHECK_INT(bad, 0);
}

void test_config(void) {
    schema_sanity();
    round_trips();
    committed_files();
    odd_input();
    merge_and_rules();
    locale_proof();
    fuzz();
}

/* Scripts/test-core.sh --update-configs */
int test_write_configs(const char *dir) {
    return config_write_default_files(dir);
}
