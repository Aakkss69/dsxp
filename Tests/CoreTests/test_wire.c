/* Wire formats: X-Plane UDP packets, the controller output report, JSON. */
#include "tests.h"
#include <stdlib.h>
#include <string.h>

void test_packets(void) {
    unsigned char p[1024];
    float f;

    size_t n = xp_build_dref(p, "sim/joystick/yoke_roll_ratio", 0.25f);
    CHECK_INT(n, 509);
    CHECK(!memcmp(p, "DREF\0", 5));
    memcpy(&f, p + 5, 4);
    CHECK_NEAR(f, 0.25, 1e-7);
    CHECK(!strcmp((char *)p + 9, "sim/joystick/yoke_roll_ratio"));

    char longp[700];
    memset(longp, 'x', sizeof longp - 1);
    longp[sizeof longp - 1] = 0;
    xp_build_dref(p, longp, 1.0f);
    CHECK_INT(strlen((char *)p + 9), 499);
    CHECK_INT(p[508], 0);

    n = xp_build_cmnd(p, "sim/general/zoom_in");
    CHECK_INT(n, 5 + strlen("sim/general/zoom_in") + 1);    /* trailing NUL */
    CHECK(!memcmp(p, "CMND\0sim/general/zoom_in", n));
    CHECK_INT(xp_build_cmnd(p, longp), 505);
    CHECK_INT(p[504], 0);

    n = xp_build_rref(p, 10, 7, "sim/flightmodel/forces/g_nrml");
    CHECK_INT(n, 413);
    int32_t v;
    memcpy(&v, p + 5, 4); CHECK_INT(v, 10);
    memcpy(&v, p + 9, 4); CHECK_INT(v, 7);
    CHECK(!strcmp((char *)p + 13, "sim/flightmodel/forces/g_nrml"));

    rref_pair_t pairs[8];
    unsigned char r[5 + 16 + 3];
    memcpy(r, "RREF,", 5);
    int32_t id = 1; float val = 88.5f;
    memcpy(r + 5, &id, 4); memcpy(r + 9, &val, 4);
    id = 7; val = 1.0f;
    memcpy(r + 13, &id, 4); memcpy(r + 17, &val, 4);
    CHECK_INT(telemetry_parse(r, sizeof r, pairs, 8), 2);   /* 3 trailing bytes ignored */
    CHECK_INT(pairs[0].id, 1); CHECK_NEAR(pairs[0].value, 88.5, 1e-6);
    CHECK_INT(pairs[1].id, 7);
    CHECK_INT(telemetry_parse(r, 12, pairs, 8), 0);
    r[0] = 'X';
    CHECK_INT(telemetry_parse(r, sizeof r, pairs, 8), 0);
}

void test_output(void) {
    const unsigned char nine[] = "123456789";
    CHECK((crc32_le(nine, 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu) == 0xCBF43926u);

    out_frame_t f;
    memset(&f, 0, sizeof f);
    unsigned char rep[OUT_REPORT_LEN];
    out_build_report(&f, 0, rep);
    CHECK_INT(rep[0], 0x31); CHECK_INT(rep[1], 0x00); CHECK_INT(rep[2], 0x10);
    CHECK_INT(rep[3], 0x0F); CHECK_INT(rep[4], 0x14);
    for (int i = 5; i < 74; i++) CHECK_INT(rep[i], 0);
    /* computed independently with Python zlib.crc32(b'\xa2' + rep[:74]) */
    CHECK_INT(rep[74], 0x71); CHECK_INT(rep[75], 0x89); CHECK_INT(rep[76], 0x8D); CHECK_INT(rep[77], 0x07);

    out_build_report(&f, 0x13, rep);
    CHECK_INT(rep[1], 0x30);                              /* low nibble of seq, shifted up */

    unsigned char e[11];
    trigger_fx_t fx = { TFX_RESIST, 0, 0, 255 };
    put_effect(e, &fx);
    unsigned char want[7] = { 0x21, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x3F };
    CHECK(!memcmp(e, want, 7));
    fx = (trigger_fx_t){ TFX_WEAPON, 0, 0, 0 };
    put_effect(e, &fx);
    CHECK_INT(e[0], 0x25); CHECK_INT(e[1], 0x0C); CHECK_INT(e[2], 0x00); CHECK_INT(e[3], 0);
    fx = (trigger_fx_t){ TFX_VIBRATE, 50, 0, 200 };
    put_effect(e, &fx);
    CHECK_INT(e[0], 0x00);                                /* zero Hz means off */
    fx.end = 30;
    put_effect(e, &fx);
    CHECK_INT(e[0], 0x26); CHECK_INT(e[9], 30);

    /* weapon with a start near full travel still yields start < stop <= 8 (F11) */
    fx = (trigger_fx_t){ TFX_WEAPON, 255, 255, 128 };
    put_effect(e, &fx);
    { int marks = e[1] | (e[2] << 8), lo = -1, hi = -1;
      for (int z = 0; z < 10; z++) if (marks & (1 << z)) { if (lo < 0) lo = z; hi = z; }
      CHECK(lo >= 2 && lo <= 7 && hi > lo && hi <= 8); }

    /* fx_equal ignores the padding byte (F12) */
    { trigger_fx_t a, b;
      memset(&a, 0xAA, sizeof a); memset(&b, 0x55, sizeof b);
      a.mode = b.mode = TFX_RESIST; a.start = b.start = 1; a.end = b.end = 2; a.strength = b.strength = 3;
      CHECK(fx_equal(&a, &b));
      b.strength = 4;
      CHECK(!fx_equal(&a, &b)); }

    f.rumble_v2 = true;
    out_build_report(&f, 0, rep);
    CHECK_INT(rep[3] & 0x03, 0);
    CHECK_INT(rep[41] & 0x04, 0x04);

    memset(&f, 0, sizeof f);
    f.light_on = false; f.r = 200; f.player = 4;
    out_build_report(&f, 0, rep);
    CHECK_INT(rep[46], 0); CHECK_INT(rep[47], 0);        /* light off sends zeros */
    f.player_force = true;                               /* a DSXP command's flash */
    out_build_report(&f, 0, rep);
    CHECK_INT(rep[46], 4); CHECK_INT(rep[47], 0);        /* lamps on, the bar still dark */
}

/* Commands.txt: path ends at the first space or tab (F10) */
static void test_commands(void) {
    const char *t = "sim/a/b  Two spaces.\nsim/audio_panel_copilot/monitor_audio_com_auto_off Copilot: one space.\n"
                    "sim/c\tTabbed\nsim/d\n";
    xcmd_t *c = NULL;
    int n = commands_parse(t, strlen(t), &c);
    CHECK_INT(n, 4);
    if (n == 4) {
        CHECK(!strcmp(c[0].path, "sim/a/b"));  CHECK(!strcmp(c[0].desc, "Two spaces."));
        CHECK(!strcmp(c[1].path, "sim/audio_panel_copilot/monitor_audio_com_auto_off"));
        CHECK(!strcmp(c[1].desc, "Copilot: one space."));
        CHECK(!strcmp(c[2].path, "sim/c"));    CHECK(!strcmp(c[2].desc, "Tabbed"));
        CHECK(!strcmp(c[3].path, "sim/d"));    CHECK(c[3].desc[0] == 0);
    }
    free(c);
}

void test_json(void) {
    test_commands();
    char out[64];
    json_escape("a\"b\\c\n", out, sizeof out);
    CHECK(!strcmp(out, "a\\\"b\\\\c\\u000a"));
    size_t n = json_escape("\"\"\"", out, 4);
    CHECK_INT(n, 2);
    CHECK(!strcmp(out, "\\\""));
    n = json_escape("\x01", out, 6);
    CHECK_INT(n, 0);
    CHECK(out[0] == 0);

    jbuf_t j;
    jb_init(&j, 8);
    for (int i = 0; i < 2000; i++) jb_printf(&j, "%05d", i);
    CHECK_INT(j.len, 10000);
    CHECK(!j.oom);
    jb_free(&j);

    jb_init(&j, 8);
    jb_str(&j, "line\nquote\"");
    CHECK(!strcmp(j.b, "\"line\\u000aquote\\\"\""));
    CHECK(json_valid(j.b, j.len));
    jb_free(&j);

    url_decode("%41%zz+b%4", out, sizeof out);
    CHECK(!strcmp(out, "A%zz b%4"));
    CHECK(!strcmp(qs_get("xinv=1&inv=0", "inv", out, sizeof out), "0"));
    CHECK(qs_get("xinv=1", "inv", out, sizeof out) == NULL);
    CHECK(!strcmp(qs_get("cmd=sim%2Fa%2Fb", "cmd", out, sizeof out), "sim/a/b"));

    const char *good = "{\"a\":[1,2.5,-3e2,true,null,\"x\"]}";
    const char *bad1 = "{\"a\":}";
    const char *bad2 = "{\"a\":\"\x01\"}";
    CHECK(json_valid(good, strlen(good)));
    CHECK(!json_valid(bad1, strlen(bad1)));
    CHECK(!json_valid(bad2, strlen(bad2)));
}
