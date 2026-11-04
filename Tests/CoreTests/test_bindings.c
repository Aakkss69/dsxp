/* bindings.conf: round trip, legacy files, hostile input, fuzzing. */
#include "tests.h"
#include <stdlib.h>
#include <string.h>

static void check_invariants(const settings_t *s) {
    CHECK(s->tune.deadzone >= 0 && s->tune.deadzone <= 0.45);
    CHECK(s->tune.curve >= 0.4 && s->tune.curve <= 3.0);
    CHECK(s->tune.gesture >= 0 && s->tune.gesture <= 1);
    CHECK(s->light.pattern >= 0 && s->light.pattern < LP_COUNT);
    CHECK(s->light.speed >= 0 && s->light.speed <= 1);
    CHECK(s->light.bright >= 0 && s->light.bright <= 1);
    CHECK(s->light.player <= 0x1F);
    CHECK(s->light.mode == LM_MANUAL || s->light.mode == LM_SIM);
    for (int i = 0; i < 2; i++) CHECK(s->fx_manual[i].mode >= TFX_OFF && s->fx_manual[i].mode <= TFX_VIBRATE);
    for (int i = 0; i < S_COUNT; i++) CHECK(s->axis[i].target >= 0 && s->axis[i].target < AX_TARGET_COUNT);
    for (int i = 0; i < B_COUNT; i++) CHECK(!s->bind[i].cmd[0] || cmd_valid(s->bind[i].cmd));
}

static bool same(const settings_t *a, const settings_t *b) {
    if (memcmp(a->bind, b->bind, sizeof a->bind)) return false;
    for (int i = 0; i < S_COUNT; i++)
        if (a->axis[i].target != b->axis[i].target || a->axis[i].invert != b->axis[i].invert) return false;
    if (fabs(a->tune.deadzone - b->tune.deadzone) > 1e-4 || fabs(a->tune.curve - b->tune.curve) > 1e-4 ||
        fabs(a->tune.gesture - b->tune.gesture) > 1e-4) return false;
    const light_t *x = &a->light, *y = &b->light;
    if (x->r != y->r || x->g != y->g || x->b != y->b || x->on != y->on || x->pattern != y->pattern ||
        fabs(x->speed - y->speed) > 1e-3 || fabs(x->bright - y->bright) > 1e-3 ||
        x->mode != y->mode || x->player != y->player) return false;
    if (memcmp(a->fx_manual, b->fx_manual, sizeof a->fx_manual)) return false;
    return a->fxmode == b->fxmode && a->rmode == b->rmode && a->rumble_v2 == b->rumble_v2;
}

static void parse_str(settings_t *s, const char *text) {
    load_defaults(s);
    bindings_parse(s, text, strlen(text));
}

void test_bindings(void) {
    settings_t a, b;
    char buf[16384];

    /* defaults survive a round trip */
    load_defaults(&a);
    size_t n = bindings_format(&a, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf);
    load_defaults(&b);
    bindings_parse(&b, buf, n);
    CHECK(same(&a, &b));
    CHECK(!b.bind[B_DP_UP].cmd[0]);                 /* D-pad Up deliberately unbound */
    CHECK(!strcmp(b.bind[B_DP_R].cmd, "sim/flight_controls/landing_gear_down"));

    /* a customised configuration survives too */
    load_defaults(&a);
    snprintf(a.bind[B_TP_SWIPE_UL].cmd, CMD_PATH_MAX, "sim/view/forward_with_2d_panel");
    a.bind[B_TP_SWIPE_UL].repeat = true;
    a.axis[S_RY] = (axis_map_t){ AX_THROTTLE, true };
    a.tune = (tuning_t){ 0.1234, 2.5, 0.75 };
    a.light = (light_t){ 1, 2, 3, false, LP_RAINBOW, 0.25, 0.5, LM_SIM, 0x1B };
    a.fx_manual[1] = (trigger_fx_t){ TFX_WEAPON, 10, 200, 99 };
    a.fxmode = FXM_WIND; a.rmode = RM_SIM; a.rumble_v2 = true;
    n = bindings_format(&a, buf, sizeof buf);
    load_defaults(&b);
    bindings_parse(&b, buf, n);
    CHECK(same(&a, &b));

    /* the prototype's file format (fake commands) loads unchanged */
    parse_str(&b,
        "# dsxp bindings - rewritten by the mapper UI\n"
        "tune deadzone 0.0550\ntune curve 1.0000\ntune gesture 0.5200\n"
        "light 139 92 246 1 5 0.340 14 1.000 1\n"
        "fxmode wind\nrumblemode manual\nrumblepath classic\n"
        "trigger l 1 30 0 120\ntrigger r 0 0 0 0\n"
        "# axis <source> <target> <invert>\n"
        "axis lx roll 0\naxis ly pitch 0\naxis rx yaw 0\naxis ry none 0\n"
        "axis l2 rudder_left 0\naxis r2 rudder_right 0\n"
        "# button <slot> <repeat> <command>\n"
        "button 0 0 sim/flight_controls/flaps_up\n"
        "button 25 1 sim/general/rot_left\n");
    CHECK_NEAR(b.tune.deadzone, 0.055, 1e-9);
    CHECK_INT(b.light.r, 139); CHECK_INT(b.light.pattern, LP_ALERT); CHECK_INT(b.light.player, 14);
    CHECK_INT(b.light.mode, LM_SIM);
    CHECK_INT(b.fxmode, FXM_WIND);
    CHECK_INT(b.fx_manual[0].mode, TFX_RESIST); CHECK_INT(b.fx_manual[0].strength, 120);
    CHECK(!strcmp(b.bind[B_DP_UP].cmd, "sim/flight_controls/flaps_up"));
    CHECK(b.bind[B_TP_SWIPE_L].repeat);
    CHECK(!b.bind[B_DP_R].cmd[0]);                  /* file is authoritative for buttons */

    /* hostile values are clamped or ignored, never trusted */
    parse_str(&b, "tune curve nan\ntune deadzone inf\ntune gesture -3\n");
    CHECK_NEAR(b.tune.curve, 1.0, 1e-12);
    CHECK_NEAR(b.tune.deadzone, 0.06, 1e-12);
    CHECK_NEAR(b.tune.gesture, 0.0, 1e-12);
    parse_str(&b, "tune curve -5\ntune deadzone 9\n");
    CHECK_NEAR(b.tune.curve, 0.4, 1e-12);
    CHECK_NEAR(b.tune.deadzone, 0.45, 1e-12);
    parse_str(&b, "light 999 -5 300 1 77 9.0 999 -1 5\n");
    CHECK_INT(b.light.r, 255); CHECK_INT(b.light.g, 0); CHECK_INT(b.light.b, 255);
    CHECK_INT(b.light.pattern, LP_SOLID); CHECK_NEAR(b.light.speed, 1.0, 1e-12);
    CHECK_INT(b.light.player, 31); CHECK_NEAR(b.light.bright, 0.0, 1e-12);
    CHECK_INT(b.light.mode, LM_MANUAL);
    parse_str(&b, "trigger x 1 2 3 4\ntrigger r 9 300 -4 128\n");
    CHECK_INT(b.fx_manual[0].mode, TFX_OFF);
    CHECK_INT(b.fx_manual[1].mode, TFX_OFF); CHECK_INT(b.fx_manual[1].start, 255);
    CHECK_INT(b.fx_manual[1].end, 0); CHECK_INT(b.fx_manual[1].strength, 128);
    parse_str(&b, "button 999 0 sim/a\nbutton -1 0 sim/b\nbutton 3 0 sim/x\"y\n"
                  "button 4 1 sim/ok\r\naxis zz roll 0\naxis lx warp 1\n");
    for (int i = 0; i < B_COUNT; i++) if (i != 4) CHECK(!b.bind[i].cmd[0]);
    CHECK(!strcmp(b.bind[4].cmd, "sim/ok"));        /* CRLF handled */
    CHECK_INT(b.axis[S_LX].target, AX_ROLL);

    /* an embedded NUL rejects only its own line; an oversized line is skipped */
    char nul[] = "button 1 0 sim/a\0b\nbutton 2 0 sim/two\n";
    load_defaults(&b);
    bindings_parse(&b, nul, sizeof nul - 1);
    CHECK(!b.bind[1].cmd[0]);
    CHECK(!strcmp(b.bind[2].cmd, "sim/two"));
    char *big = malloc(5100);
    memset(big, 'a', 4990);
    memcpy(big, "button 5 0 sim/", 15);
    memcpy(big + 4990, "\nbutton 6 0 sim/six\n", 20);
    load_defaults(&b);
    bindings_parse(&b, big, 4990 + 20);
    CHECK(!b.bind[5].cmd[0]);
    CHECK(!strcmp(b.bind[6].cmd, "sim/six"));
    free(big);

    CHECK(cmd_valid("sim/flight_controls/flaps_up"));
    CHECK(!cmd_valid(""));
    CHECK(!cmd_valid("sim/a b"));
    CHECK(!cmd_valid("sim/a\nbutton 1 0 x"));

    /* random mutation: whatever the bytes, the result must satisfy the invariants */
    load_defaults(&a);
    n = bindings_format(&a, buf, sizeof buf);
    uint32_t x = 2463534242u;
    char m[16384];
    int bad = 0;
    for (int it = 0; it < 100000; it++) {
        memcpy(m, buf, n);
        int k = 1 + (int)(x % 8);
        for (int j = 0; j < k; j++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            size_t pos = x % n;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            m[pos] = (char)(x & 0xFF);
        }
        load_defaults(&b);
        bindings_parse(&b, m, n);
        int before = t_failures;
        check_invariants(&b);
        if (t_failures != before) { bad++; if (bad > 3) break; }
    }
    CHECK_INT(bad, 0);
}
