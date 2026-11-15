/* The JSON API: methods, validation, escaping, mode guards. Runs against a real
 * core_t with commands applied synchronously. */
#include "tests.h"
#include <string.h>

static core_t AC;
static double AT = 100.0;

static void a_snap(void *ctx, dsxp_snap_t *o) { (void)ctx; core_snapshot(&AC, AT, o); }
static bool a_submit(void *ctx, const cmd_t *c) { (void)ctx; core_apply_cmd(&AC, c, AT); return true; }
static void a_cat(void *ctx, void (*fn)(void *, const xcmd_t *, int), void *arg) {
    (void)ctx;
    static const xcmd_t cm[2] = {
        { "sim/a/b", "Desc with \"quotes\" and \\ backslash\t tab" },
        { "sim/c", "plain" },
    };
    fn(arg, cm, 2);
}
static void a_app(void *ctx, jbuf_t *j) { (void)ctx; jb_printf(j, "{\"access\":\"granted\"}"); }

static const api_env_t ENV = { NULL, a_snap, a_submit, a_cat, a_app };

static int call(const char *m, const char *p, const char *q) {
    dsxp_response_t r;
    api_handle(&ENV, m, p, q, &r);
    int s = r.status;
    dsxp_response_free(&r);
    return s;
}

static bool body_has(const char *m, const char *p, const char *q, const char *needle, bool *valid) {
    dsxp_response_t r;
    api_handle(&ENV, m, p, q, &r);
    bool has = r.body && strstr(r.body, needle) != NULL;
    if (valid) *valid = r.body && json_valid(r.body, r.len);
    dsxp_response_free(&r);
    return has;
}

void test_api(void) {
    settings_t s;
    load_defaults(&s);
    core_init(&AC, &s, NULL);

    /* methods and routes */
    CHECK_INT(call("GET", "/api/bind", "i=1"), 405);
    CHECK_INT(call("POST", "/api/state", ""), 405);
    CHECK_INT(call("GET", "/api/quit", ""), 404);          /* quit is the app's, not the core's */
    CHECK_INT(call("GET", "/nope", ""), 404);
    CHECK(body_has("GET", "/nope", "", "no route", NULL));
    CHECK(!body_has("GET", "/nope", "", "/", NULL));       /* no paths leak into errors */

    /* bind */
    CHECK_INT(call("POST", "/api/bind", "i=99&cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=-1&cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=3abc&cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=3&cmd=sim%00junk"), 400);   /* not bound as "sim" */
    CHECK_INT(call("POST", "/api/bind", "i=99999999999999999999&cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=-&cmd=sim%2Fa"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=3&cmd=a%0Abutton%201%200%20x"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=3&cmd=sim%2Fx%22y"), 400);
    CHECK_INT(call("POST", "/api/bind", "i=3&cmd=sim%2Fx%2Fy&repeat=1"), 200);
    CHECK(!strcmp(AC.s.bind[3].cmd, "sim/x/y"));
    CHECK(AC.s.bind[3].repeat);
    CHECK_INT(call("POST", "/api/bind", "i=3&cmd="), 200);
    CHECK(!AC.s.bind[3].cmd[0]);

    /* tune: NaN is ignored, ranges are clamped */
    double dz = AC.s.tune.deadzone;
    CHECK_INT(call("POST", "/api/tune", "deadzone=nan&curve=2"), 200);
    CHECK_NEAR(AC.s.tune.deadzone, dz, 1e-12);
    CHECK_NEAR(AC.s.tune.curve, 2.0, 1e-12);
    CHECK_INT(call("POST", "/api/tune", "deadzone=0.9&curve=0.1&gesture=7"), 200);
    CHECK_NEAR(AC.s.tune.deadzone, 0.45, 1e-12);
    CHECK_NEAR(AC.s.tune.curve, 0.4, 1e-12);
    CHECK_NEAR(AC.s.tune.gesture, 1.0, 1e-12);

    /* light: clamped, and only the fields sent change */
    unsigned char g0 = AC.s.light.g;
    CHECK_INT(call("POST", "/api/light", "r=999&player=255&pattern=9"), 200);
    CHECK_INT(AC.s.light.r, 255);
    CHECK_INT(AC.s.light.player, 31);
    CHECK_INT(AC.s.light.pattern, LP_SOLID);
    CHECK_INT(AC.s.light.g, g0);

    /* axis */
    CHECK_INT(call("POST", "/api/axis", "k=lx&t=throttle&inv=1"), 200);
    CHECK_INT(AC.s.axis[S_LX].target, AX_THROTTLE);
    CHECK(AC.s.axis[S_LX].invert);
    CHECK_INT(call("POST", "/api/axis", "k=zz&t=roll"), 400);
    CHECK_INT(call("POST", "/api/axis", "k=lx&t=warp"), 400);

    /* mode guards */
    CHECK_INT(call("POST", "/api/triggersource", ""), 400);
    CHECK_INT(call("POST", "/api/triggersource", "source=wind"), 200);
    CHECK_INT(call("POST", "/api/trigger", "side=q&mode=1"), 400);
    CHECK_INT(call("POST", "/api/trigger", "side=l&mode=1"), 409);
    CHECK_INT(call("POST", "/api/triggersource", "source=manual"), 200);
    CHECK_INT(call("POST", "/api/trigger", "side=r&mode=2&start=10&end=999&strength=50"), 200);
    CHECK_INT(AC.s.fx_manual[1].mode, TFX_WEAPON);
    CHECK_INT(AC.s.fx_manual[1].end, 255);
    /* vibrate keeps its frequency apart from the weapon band (step 8) */
    CHECK_INT(call("POST", "/api/trigger", "side=r&mode=3&freq=30"), 200);
    CHECK_INT(AC.s.fx_manual[1].freq, 30);
    CHECK_INT(AC.s.fx_manual[1].end, 255);
    CHECK_INT(AC.fx_out[1].end, 30);                  /* on the wire: end carries the Hz */
    CHECK(body_has("GET", "/api/state", "", "\"trigger_manual\":[", NULL));
    CHECK_INT(call("POST", "/api/rumblesource", "source=sim"), 200);
    CHECK_INT(call("POST", "/api/rumble", "left=10"), 409);
    CHECK_INT(call("POST", "/api/rumblesource", "source=manual"), 200);
    CHECK_INT(call("POST", "/api/rumble", "left=300&right=-4"), 200);
    CHECK_INT(AC.rumble.left, 255);
    CHECK_INT(AC.rumble.right, 0);
    CHECK_INT(call("POST", "/api/rumblepath", "v2=1"), 200);
    CHECK(AC.s.rumble_v2);

    /* state and init are always valid JSON, strings escaped */
    snprintf(AC.rumble_note, sizeof AC.rumble_note, "odd \"note\"\n");
    bool valid = false;
    CHECK(body_has("GET", "/api/state", "", "\"app\":{\"access\":\"granted\"}", &valid));
    CHECK(valid);
    CHECK(body_has("GET", "/api/init", "", "Desc with \\\"quotes\\\" and \\\\ backslash\\u0009 tab", &valid));
    CHECK(valid);
    CHECK(body_has("GET", "/api/init", "", "\"n\":\"Swipe down-right\"", NULL));

    /* motion: DSXP's own commands lead the catalog, the X-Plane ones follow */
    CHECK(body_has("GET", "/api/init", "", "{\"commands\":[{\"c\":\"dsxp/motion/recenter\"", &valid));
    CHECK(valid);
    CHECK(body_has("GET", "/api/init", "", "\"c\":\"dsxp/motion/hold\"", NULL));
    CHECK(body_has("GET", "/api/init", "", "},{\"c\":\"sim/a/b\"", NULL));   /* comma-joined */

    /* motion settings: any subset; a present but bad value is refused whole */
    CHECK_INT(call("GET", "/api/motion", "enabled=1"), 405);
    CHECK_INT(call("POST", "/api/motion", "roll=tilt"), 400);
    CHECK_INT(call("POST", "/api/motion", "enabled=yes"), 400);
    CHECK_INT(call("POST", "/api/motion", "enabled=1&pitch.full=abc"), 400);
    CHECK_INT(call("POST", "/api/motion", "yaw.invert="), 400);
    CHECK(!AC.s.motion.enabled);                                   /* nothing applied   */
    CHECK_INT(call("POST", "/api/motion", "enabled=1"), 409);       /* not calibrated yet */
    CHECK(body_has("GET", "/api/state", "", "\"calibrated\":0,\"calibration\":{\"step\":\"idle\"", &valid));
    CHECK(valid);
    CHECK_INT(call("POST", "/api/calibrate", "action=close"), 409);  /* required first      */
    CHECK_INT(call("POST", "/api/calibrate", "action=later"), 400);
    CHECK_INT(call("POST", "/api/calibrate", "action=start"), 200);
    CHECK(body_has("GET", "/api/state", "", "\"calibration\":{\"step\":\"flat\"", NULL));
    CHECK_INT(call("POST", "/api/calibrate", "action=abort"), 200);  /* allowed even before */
    CHECK(body_has("GET", "/api/state", "", "\"calibration\":{\"step\":\"idle\"", NULL));   /* the first one */
    CHECK_INT(call("POST", "/api/calibrate", "action=start"), 200);
    AC.s.motion.calibrated = true;                                 /* as if it finished */
    CHECK_INT(call("POST", "/api/calibrate", "action=close"), 200);
    CHECK(body_has("GET", "/api/state", "", "\"calibration\":{\"step\":\"idle\"", NULL));
    CHECK_INT(call("POST", "/api/motion", "enabled=1&yaw=motion&roll=sticks&roll.full=55&pitch.invert=1"), 200);
    CHECK(AC.s.motion.enabled);
    CHECK(AC.s.motion.use[MOTION_YAW] && !AC.s.motion.use[MOTION_ROLL] && AC.s.motion.use[MOTION_PITCH]);
    CHECK_NEAR(AC.s.motion.full_deg[MOTION_ROLL], 55, 1e-12);
    CHECK(AC.s.motion.invert[MOTION_PITCH] && !AC.s.motion.invert[MOTION_ROLL]);
    CHECK_INT(call("POST", "/api/motion", "yaw.full=1000"), 200);   /* clamped by the core */
    CHECK_NEAR(AC.s.motion.full_deg[MOTION_YAW], 90, 1e-12);
    CHECK_INT(call("POST", "/api/motionrecenter", ""), 200);
    CHECK_INT(call("GET", "/api/motionrecenter", ""), 405);

    /* ... and in /api/state, still valid JSON */
    CHECK(body_has("GET", "/api/state", "", "\"motion\":{\"enabled\":1,\"scales\":\"none\"", &valid));
    CHECK(valid);
    CHECK(body_has("GET", "/api/state", "", "{\"use\":\"sticks\",\"full\":55,\"invert\":0,", NULL));
    CHECK(body_has("GET", "/api/state", "", "\"sent\":{\"valid\":0,", NULL));
}
