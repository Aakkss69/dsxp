/*
 * The mapper UI's JSON API, transport-independent: the app serves it through a WKWebView
 * custom-scheme handler, so no TCP port is ever opened.
 *
 *   GET  /api/init          command catalog + slot/source/target vocabulary
 *   GET  /api/state         live axes, buttons, gate, bindings, tuning, app info
 *   POST /api/bind          ?i=<slot>&cmd=<path>&repeat=<0|1>   (empty cmd clears)
 *   POST /api/axis          ?k=<source>&t=<target>&inv=<0|1>
 *   POST /api/tune          ?deadzone=<f>&curve=<f>&gesture=<f>
 *   POST /api/light         ?r=&g=&b=&on=<0|1>&pattern=<0-5>&speed=<f>&player=<0-31>&bright=<f>&source=<manual|sim>
 *   POST /api/trigger       ?side=<l|r>&mode=<0-3>&start=&end=&strength=&freq=   (manual source only)
 *   POST /api/triggersource ?source=<manual|wind>
 *   POST /api/rumble        ?left=&right=                                       (manual source only)
 *   POST /api/rumblesource  ?source=<manual|sim>
 *   POST /api/rumblepath    ?v2=<0|1>
 *   POST /api/motion        ?enabled=<0|1>&roll=<sticks|motion>&pitch=..&yaw=..
 *                           &roll.full=<deg>&pitch.full=..&yaw.full=..&roll.invert=<0|1>..
 *                           (any subset; the words and keys match controls.conf)
 *   POST /api/motionrecenter                      the controller's attitude now is level
 *   POST /api/calibrate     ?action=<start|close>  run the motion calibration (again), or
 *                           close it (refused until motion control has been calibrated once)
 * The words match the config files: trigger.source, rumble.source, light.source.
 * /api/quit, /api/pickxp and /api/openaccess are handled by the app itself.
 */
#include "core_internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CT_JSON = "application/json";
static const char *CT_TEXT = "text/plain; charset=utf-8";

static void respond(dsxp_response_t *out, int status, const char *ctype, const char *body) {
    size_t n = strlen(body);
    out->status = status;
    out->content_type = ctype;
    out->body = malloc(n + 1);
    if (out->body) { memcpy(out->body, body, n + 1); out->len = n; }
    else out->len = 0;
}

static void respond_json(dsxp_response_t *out, jbuf_t *j) {
    if (j->oom || !j->b) { jb_free(j); respond(out, 500, CT_TEXT, "out of memory"); return; }
    out->status = 200;
    out->content_type = CT_JSON;
    out->body = j->b;
    out->len = j->len;
    j->b = NULL;
}

static void ok(dsxp_response_t *out) { respond(out, 200, CT_JSON, "{\"ok\":true}"); }

/* CERT ERR34-C: reject empty input, trailing junk ("12abc") and overflow. */
static bool get_long(const char *qs, const char *key, long *v) {
    char buf[64];
    if (!qs_get(qs, key, buf, sizeof buf) || !buf[0]) return false;
    char *e;
    errno = 0;
    long x = strtol(buf, &e, 10);
    if (e == buf || *e != 0 || errno == ERANGE) return false;
    *v = x;
    return true;
}

static bool get_double(const char *qs, const char *key, double *v) {
    char buf[64];
    if (!qs_get(qs, key, buf, sizeof buf) || !buf[0]) return false;
    char *e;
    errno = 0;
    double x = strtod(buf, &e);
    if (e == buf || *e != 0 || errno == ERANGE || !isfinite(x)) return false;
    *v = x;
    return true;
}

static void submit(const api_env_t *env, const cmd_t *cmd, dsxp_response_t *out) {
    if (env->submit && env->submit(env->ctx, cmd)) ok(out);
    else respond(out, 503, CT_TEXT, "busy");
}

/* ------------------------------------------------------------- GET routes */

typedef struct { jbuf_t *j; int n; } init_arg_t;   /* n: commands written so far */

static void init_commands(void *arg, const xcmd_t *cmds, int n) {
    init_arg_t *a = arg;
    for (int i = 0; i < n; i++) {
        jb_printf(a->j, "%s{\"c\":", a->n++ ? "," : "");   /* comma before all but the first */
        jb_str(a->j, cmds[i].path);
        jb_printf(a->j, ",\"d\":");
        jb_str(a->j, cmds[i].desc);
        jb_printf(a->j, "}");
    }
}

static void api_init(const api_env_t *env, dsxp_response_t *out) {
    jbuf_t j;
    jb_init(&j, 512 * 1024);
    jb_printf(&j, "{\"commands\":[");
    init_arg_t arg = { &j, 0 };
    init_commands(&arg, DSXP_COMMANDS, DSXP_CMD_COUNT);       /* DSXP's own commands first */
    if (env->catalog) env->catalog(env->ctx, init_commands, &arg);
    jb_printf(&j, "],\"slots\":[");
    for (int i = 0; i < B_COUNT; i++) {
        jb_printf(&j, "%s{\"i\":%d,\"n\":", i ? "," : "", i);
        jb_str(&j, BTN_NAME[i]);
        jb_printf(&j, "}");
    }
    jb_printf(&j, "],\"sources\":[");
    for (int i = 0; i < S_COUNT; i++) {
        jb_printf(&j, "%s{\"k\":", i ? "," : "");
        jb_str(&j, SRC_KEY[i]);
        jb_printf(&j, ",\"n\":");
        jb_str(&j, SRC_LABEL[i]);
        jb_printf(&j, "}");
    }
    jb_printf(&j, "],\"targets\":[");
    for (int i = 0; i < AX_TARGET_COUNT; i++) {
        jb_printf(&j, "%s{\"k\":", i ? "," : "");
        jb_str(&j, AX_KEY[i]);
        jb_printf(&j, ",\"n\":");
        jb_str(&j, AX_LABEL[i]);
        jb_printf(&j, "}");
    }
    jb_printf(&j, "]}");
    respond_json(out, &j);
}

/* "motion": the settings, the live angles and outputs, the calibration, and what X-Plane
 * was sent (the mapper's 3D plane draws only that). Axis order: roll, pitch, yaw.
 * Two different calibrations appear, named apart: "scales" says where the sensor scales
 * come from (the controller's factory calibration, report 0x05, or nominal values), while
 * "calibrated" and "calibration" are the user's routine (motion_calib.c). */
static void state_motion(jbuf_t *j, const dsxp_snap_t *s) {
    static const char *const SCALES[] = { "none", "nominal", "factory" };   /* motion_cal_source_t */
    int scales = s->motion_cal_src >= 0 && s->motion_cal_src <= 2 ? s->motion_cal_src : 0;
    const motion_cfg_t *m = &s->motion;
    jb_printf(j, "\"motion\":{\"enabled\":%d,\"scales\":\"%s\",\"live\":%d,\"out_of_range\":%d,"
                 "\"bias_learned\":%d,\"hold\":%d,\"deadzone\":%.1f,\"yaw_deadzone\":%.1f,"
                 "\"axes\":[",
              m->enabled ? 1 : 0, SCALES[scales], s->motion_seeded ? 1 : 0, s->motion_out_of_range ? 1 : 0,
              s->motion_bias_learned ? 1 : 0, s->motion_hold ? 1 : 0,
              m->deadzone_deg, m->yaw_deadzone_deg);
    for (int i = 0; i < MOTION_AXES; i++)
        jb_printf(j, "%s{\"use\":\"%s\",\"full\":%.0f,\"invert\":%d,\"angle\":%.2f,\"out\":%.4f}",
                  i ? "," : "", m->use[i] ? "motion" : "sticks", m->full_deg[i], m->invert[i] ? 1 : 0,
                  isfinite(s->motion_angle[i]) ? s->motion_angle[i] : 0.0,
                  isfinite(s->motion_out[i]) ? s->motion_out[i] : 0.0);
    jb_printf(j, "],\"sent\":{\"valid\":%d,\"roll\":%.4f,\"pitch\":%.4f,\"yaw\":%.4f},",
              s->sent_valid ? 1 : 0, s->sent[MOTION_ROLL], s->sent[MOTION_PITCH], s->sent[MOTION_YAW]);
    /* the calibration: whether it has been done, and the routine's progress */
    const calib_t *k = &s->calib;
    jb_printf(j, "\"calibrated\":%d,\"calibration\":{\"step\":\"%s\",\"hold\":%.2f,\"need\":%.2f,"
                 "\"angle\":%.1f,\"lo\":%.1f,\"hi\":%.1f,\"min\":%.0f,\"reached\":%d,\"note\":",
              m->calibrated ? 1 : 0, calib_step_name(k->step), k->hold_s, k->need_s,
              isfinite(k->angle) ? k->angle : 0.0, k->lo, k->hi, k->min_reach, k->reached ? 1 : 0);
    jb_str(j, k->note);
    jb_printf(j, ",\"warning\":");
    jb_str(j, k->warning);
    jb_printf(j, ",\"result\":{\"bias\":[%.3f,%.3f,%.3f],\"gravity\":%.3f,\"level\":[%.1f,%.1f],"
                 "\"full\":[%.0f,%.0f,%.0f]}}}",
              k->bias[0], k->bias[1], k->bias[2], k->acc_g, k->level_bank, k->level_pitch,
              k->full[MOTION_ROLL], k->full[MOTION_PITCH], k->full[MOTION_YAW]);
}

static void api_state(const api_env_t *env, dsxp_response_t *out) {
    dsxp_snap_t *s = calloc(1, sizeof *s);
    if (!s) { respond(out, 500, CT_TEXT, "out of memory"); return; }
    if (env->snapshot) env->snapshot(env->ctx, s);

    jbuf_t j;
    jb_init(&j, 24 * 1024);
    jb_printf(&j, "{\"connected\":%s,\"reports\":%ld,\"total\":%ld,",
              s->connected ? "true" : "false", s->reports, s->total);
    jb_printf(&j, "\"tune\":{\"deadzone\":%.4f,\"curve\":%.4f,\"gesture\":%.3f},",
              s->tune.deadzone, s->tune.curve, s->tune.gesture);
    jb_printf(&j, "\"light\":{\"r\":%d,\"g\":%d,\"b\":%d,\"on\":%d,\"pattern\":%d,"
                  "\"speed\":%.3f,\"player\":%d,\"bright\":%.3f,\"source\":\"%s\",\"note\":",
              s->light.r, s->light.g, s->light.b, s->light.on ? 1 : 0, s->light.pattern,
              s->light.speed, s->light.player, s->light.bright,
              s->light.source == LIGHT_SRC_SIM ? "sim" : "manual");
    jb_str(&j, s->light_note);
    jb_printf(&j, "},\"fx\":[");
    for (int i = 0; i < 2; i++)
        jb_printf(&j, "%s{\"mode\":%d,\"start\":%d,\"end\":%d,\"strength\":%d}", i ? "," : "",
                  s->fx[i].mode, s->fx[i].start, s->fx[i].end, s->fx[i].strength);
    /* fx = what goes to the controller now; trigger_manual = the hand-set values the panel edits */
    jb_printf(&j, "],\"trigger_manual\":[");
    for (int i = 0; i < 2; i++)
        jb_printf(&j, "%s{\"mode\":%d,\"start\":%d,\"end\":%d,\"strength\":%d,\"freq\":%d}", i ? "," : "",
                  s->trigger_manual[i].mode, s->trigger_manual[i].start, s->trigger_manual[i].end, s->trigger_manual[i].strength, s->trigger_manual[i].freq);
    jb_printf(&j, "],\"trigger_source\":\"%s\",", s->trigger_source == TRIGGER_SRC_WIND ? "wind" : "manual");
    jb_printf(&j, "\"wx\":{\"live\":%d,\"ias\":%.1f,\"wind\":%.1f,\"dir\":%.0f,\"hdg\":%.0f,"
                  "\"turb\":%.2f,\"xwind\":%.1f,\"load\":%.3f,\"trigger_note\":",
              s->wx.live ? 1 : 0, s->wx.ias, s->wx.wind_ms, s->wx.wind_dir, s->wx.heading,
              s->wx.turb, s->wx.crosswind, s->wx.load);
    jb_str(&j, s->wx.trigger_note);
    jb_printf(&j, ",\"stall_l\":%.3f,\"stall_r\":%.3f,\"stall\":%.3f,\"gn\":%.2f,\"crashed\":%d},",
              s->wx.stall_l, s->wx.stall_r, s->wx.stall_ratio, s->wx.gn, s->wx.crashed ? 1 : 0);
    jb_printf(&j, "\"rumble_source\":\"%s\",\"rumble_note\":", s->rumble_source == RUMBLE_SRC_SIM ? "sim" : "manual");
    jb_str(&j, s->rumble_note);
    jb_printf(&j, ",\"rumble\":{\"left\":%d,\"right\":%d,\"v2\":%d},",
              s->rumble.left, s->rumble.right, s->rumble_v2 ? 1 : 0);
    jb_printf(&j, "\"gate\":{\"bt\":%d,\"usb\":%d,\"xp\":%d,\"engaged\":%d},",
              s->bt, s->usb, s->xp, s->engaged);
    jb_printf(&j, "\"axes\":[");
    for (int i = 0; i < S_COUNT; i++) jb_printf(&j, "%s%.4f", i ? "," : "", s->axes[i]);
    jb_printf(&j, "],\"raw\":[");
    for (int i = 0; i < S_COUNT; i++) jb_printf(&j, "%s%.4f", i ? "," : "", s->raw[i]);
    jb_printf(&j, "],\"btn\":[");
    for (int i = 0; i < B_COUNT; i++) jb_printf(&j, "%s%d", i ? "," : "", s->btn[i] ? 1 : 0);
    jb_printf(&j, "],\"bind\":[");
    for (int i = 0; i < B_COUNT; i++) {
        jb_printf(&j, "%s{\"c\":", i ? "," : "");
        jb_str(&j, s->bind[i].cmd);
        jb_printf(&j, ",\"r\":%d}", s->bind[i].repeat ? 1 : 0);
    }
    jb_printf(&j, "],\"axismap\":[");
    for (int i = 0; i < S_COUNT; i++) {
        int t = s->axis[i].target;
        if (t < 0 || t >= AX_TARGET_COUNT) t = AX_NONE;
        jb_printf(&j, "%s{\"t\":\"%s\",\"inv\":%d}", i ? "," : "", AX_KEY[t], s->axis[i].invert ? 1 : 0);
    }
    jb_printf(&j, "],\"touch\":[");
    for (int i = 0; i < 2; i++)
        jb_printf(&j, "%s{\"on\":%d,\"x\":%.4f,\"y\":%.4f}", i ? "," : "",
                  s->touch[i].active ? 1 : 0, s->touch[i].x, s->touch[i].y);
    jb_printf(&j, "],");
    state_motion(&j, s);
    jb_printf(&j, ",\"app\":");
    if (env->app_info) env->app_info(env->ctx, &j);
    else jb_printf(&j, "{}");
    jb_printf(&j, "}");
    free(s);
    respond_json(out, &j);
}

/* ------------------------------------------------------------ POST routes */

static void api_bind(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    long slot = -1, rep = 0;
    if (!get_long(qs, "i", &slot) || slot < 0 || slot >= B_COUNT) {
        respond(out, 400, CT_TEXT, "bad slot"); return;
    }
    get_long(qs, "repeat", &rep);
    char cmdbuf[1024];
    bool got = qs_get(qs, "cmd", cmdbuf, sizeof cmdbuf) != NULL;
    if (!got && qs_has(qs, "cmd")) { respond(out, 400, CT_TEXT, "bad command"); return; }   /* %00, too long */
    if (cmdbuf[0] && !cmd_valid(cmdbuf)) { respond(out, 400, CT_TEXT, "bad command"); return; }
    cmd_t c = { .kind = CMD_BIND, .key = (int)slot };
    snprintf(c.u.bind.cmd, sizeof c.u.bind.cmd, "%s", cmdbuf);   /* cmd_valid: < 256 chars */
    c.u.bind.repeat = rep != 0;
    submit(env, &c, out);
}

static void api_axis(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    char k[32], t[32];
    qs_get(qs, "k", k, sizeof k);
    qs_get(qs, "t", t, sizeof t);
    long inv = 0;
    get_long(qs, "inv", &inv);
    int si = -1, ti = -1;
    for (int i = 0; i < S_COUNT; i++) if (!strcmp(k, SRC_KEY[i])) si = i;
    for (int i = 0; i < AX_TARGET_COUNT; i++) if (!strcmp(t, AX_KEY[i])) ti = i;
    if (si < 0 || ti < 0) { respond(out, 400, CT_TEXT, "bad axis"); return; }
    cmd_t c = { .kind = CMD_AXIS, .key = si };
    c.u.axis = (axis_map_t){ ti, inv != 0 };
    submit(env, &c, out);
}

static void api_tune(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    cmd_t c = { .kind = CMD_TUNE };
    c.u.tune.has_dz = get_double(qs, "deadzone", &c.u.tune.dz);
    c.u.tune.has_cv = get_double(qs, "curve", &c.u.tune.cv);
    c.u.tune.has_gs = get_double(qs, "gesture", &c.u.tune.gs);
    submit(env, &c, out);
}

static void api_light(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    cmd_t c = { .kind = CMD_LIGHT };
    light_t *v = &c.u.light.v;
    unsigned m = 0;
    long x; double d;
    if (get_long(qs, "r", &x)) { v->r = (unsigned char)clampi(x, 0, 255); m |= LF_R; }
    if (get_long(qs, "g", &x)) { v->g = (unsigned char)clampi(x, 0, 255); m |= LF_G; }
    if (get_long(qs, "b", &x)) { v->b = (unsigned char)clampi(x, 0, 255); m |= LF_B; }
    if (get_long(qs, "on", &x)) { v->on = x != 0; m |= LF_ON; }
    if (get_long(qs, "pattern", &x)) {
        v->pattern = (x < 0 || x >= LP_COUNT) ? LP_SOLID : (int)x; m |= LF_PATTERN;
    }
    if (get_double(qs, "speed", &d)) { v->speed = clampd(d, 0, 1, 0.5); m |= LF_SPEED; }
    /* five lamps, so only the low five bits mean anything */
    if (get_long(qs, "player", &x)) { v->player = (unsigned char)(clampi(x, 0, 255) & 0x1F); m |= LF_PLAYER; }
    if (get_double(qs, "bright", &d)) { v->bright = clampd(d, 0, 1, 1.0); m |= LF_BRIGHT; }
    char src[16];
    if (qs_get(qs, "source", src, sizeof src)) {
        v->source = strcmp(src, "sim") == 0 ? LIGHT_SRC_SIM : LIGHT_SRC_MANUAL; m |= LF_SOURCE;
    }
    c.u.light.mask = m;
    submit(env, &c, out);
}

/* Adaptive trigger effect. Parameters mean different things per mode:
 *   RESIST  start = where resistance begins, strength = how hard
 *   WEAPON  start/end = the resisting band, strength = break-through force
 *   VIBRATE start = threshold, strength = amplitude, end = frequency (Hz) */
static void api_trigger(const api_env_t *env, const dsxp_snap_t *snap, const char *qs,
                        dsxp_response_t *out) {
    char side[8];
    qs_get(qs, "side", side, sizeof side);
    int si = !strcmp(side, "l") ? 0 : !strcmp(side, "r") ? 1 : -1;
    if (si < 0) { respond(out, 400, CT_TEXT, "bad side"); return; }
    if (snap->trigger_source == TRIGGER_SRC_WIND) { respond(out, 409, CT_TEXT, "Wind sets the triggers now. Switch Trigger effects to Manual to change them."); return; }
    cmd_t c = { .kind = CMD_TRIGGER, .key = si };
    unsigned m = 0;
    long x;
    if (get_long(qs, "mode", &x)) {
        c.u.trig.v.mode = (x < TFX_OFF || x > TFX_VIBRATE) ? TFX_OFF : (int)x; m |= TF_MODE;
    }
    if (get_long(qs, "start", &x))    { c.u.trig.v.start    = (unsigned char)clampi(x, 0, 255); m |= TF_START; }
    if (get_long(qs, "end", &x))      { c.u.trig.v.end      = (unsigned char)clampi(x, 0, 255); m |= TF_END; }
    if (get_long(qs, "strength", &x)) { c.u.trig.v.strength = (unsigned char)clampi(x, 0, 255); m |= TF_STRENGTH; }
    if (get_long(qs, "freq", &x))     { c.u.trig.v.freq     = (unsigned char)clampi(x, 1, 255); m |= TF_FREQ; }
    c.u.trig.mask = m;
    submit(env, &c, out);
}

static void api_trigger_source(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    char v[16];
    if (!qs_get(qs, "source", v, sizeof v) || !v[0]) { respond(out, 400, CT_TEXT, "no source"); return; }
    cmd_t c = { .kind = CMD_TRIGGER_SOURCE };
    c.u.source = strcmp(v, "wind") == 0 ? TRIGGER_SRC_WIND : TRIGGER_SRC_MANUAL;
    submit(env, &c, out);
}

static void api_rumble(const api_env_t *env, const dsxp_snap_t *snap, const char *qs,
                       dsxp_response_t *out) {
    if (snap->rumble_source == RUMBLE_SRC_SIM) { respond(out, 409, CT_TEXT, "The simulator runs the motors now. Switch Rumble to Manual to test them."); return; }
    cmd_t c = { .kind = CMD_RUMBLE };
    long x;
    if (get_long(qs, "left", &x))  { c.u.rumble.l = (unsigned char)clampi(x, 0, 255); c.u.rumble.has_l = true; }
    if (get_long(qs, "right", &x)) { c.u.rumble.r = (unsigned char)clampi(x, 0, 255); c.u.rumble.has_r = true; }
    submit(env, &c, out);
}

static void api_rumble_source(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    char v[16];
    if (!qs_get(qs, "source", v, sizeof v) || !v[0]) { respond(out, 400, CT_TEXT, "no source"); return; }
    cmd_t c = { .kind = CMD_RUMBLE_SOURCE };
    c.u.source = strcmp(v, "sim") == 0 ? RUMBLE_SRC_SIM : RUMBLE_SRC_MANUAL;
    submit(env, &c, out);
}

static void api_rumblepath(const api_env_t *env, const char *qs, dsxp_response_t *out) {
    long x;
    if (!get_long(qs, "v2", &x)) { ok(out); return; }
    cmd_t c = { .kind = CMD_RUMBLEPATH };
    c.u.flag = x != 0;
    submit(env, &c, out);
}

/* Motion settings. Every parameter is optional, but one that is present must be valid:
 * a typo must not quietly switch an axis to the sticks mid-flight. The core clamps the
 * angles to the ranges controls.conf documents. */
static void api_motion(const api_env_t *env, const dsxp_snap_t *snap, const char *qs,
                       dsxp_response_t *out) {
    static const char *const AXIS[MOTION_AXES] = { "roll", "pitch", "yaw" };   /* = config words */
    cmd_t c = { .kind = CMD_MOTION };
    motion_cmd_t *m = &c.u.motion;
    long x;
    char key[32], word[16];
    if (qs_get(qs, "enabled", word, sizeof word)) {
        if (!get_long(qs, "enabled", &x)) { respond(out, 400, CT_TEXT, "bad enabled"); return; }
        m->has_enabled = true;
        m->enabled = x != 0;
    }
    for (int i = 0; i < MOTION_AXES; i++) {
        unsigned bit = 1u << i;
        if (qs_get(qs, AXIS[i], word, sizeof word)) {                 /* roll=motion|sticks */
            if (strcmp(word, "motion") && strcmp(word, "sticks")) { respond(out, 400, CT_TEXT, "bad source"); return; }
            m->use_mask |= bit;
            m->use[i] = !strcmp(word, "motion");
        }
        snprintf(key, sizeof key, "%s.full", AXIS[i]);                 /* roll.full=<deg>    */
        if (qs_get(qs, key, word, sizeof word)) {
            if (!get_double(qs, key, &m->full[i])) { respond(out, 400, CT_TEXT, "bad angle"); return; }
            m->full_mask |= bit;
        }
        snprintf(key, sizeof key, "%s.invert", AXIS[i]);               /* roll.invert=<0|1>  */
        if (qs_get(qs, key, word, sizeof word)) {
            if (!get_long(qs, key, &x)) { respond(out, 400, CT_TEXT, "bad invert"); return; }
            m->inv_mask |= bit;
            m->inv[i] = x != 0;
        }
    }
    if (m->has_enabled && m->enabled && !snap->motion.calibrated) {   /* the core refuses too */
        respond(out, 409, CT_TEXT, "Calibrate motion control before switching it on.");
        return;
    }
    submit(env, &c, out);
}

/* The calibration routine: start it (again), close it, or abort it (the mapper's window
 * closed while it ran: core_internal.h CALIB_ACTION_ABORT). */
static void api_calibrate(const api_env_t *env, const dsxp_snap_t *snap, const char *qs,
                          dsxp_response_t *out) {
    char action[16];
    qs_get(qs, "action", action, sizeof action);
    cmd_t c = { .kind = CMD_CALIBRATE };
    if (!strcmp(action, "start")) c.key = CALIB_ACTION_START;
    else if (!strcmp(action, "close")) {
        if (!snap->motion.calibrated && snap->calib.step != CALIB_DONE) {
            respond(out, 409, CT_TEXT, "Motion control needs this calibration before first use.");
            return;
        }
        c.key = CALIB_ACTION_CLOSE;
    } else if (!strcmp(action, "abort")) c.key = CALIB_ACTION_ABORT;
    else { respond(out, 400, CT_TEXT, "bad action"); return; }
    submit(env, &c, out);
}

static void api_motion_recenter(const api_env_t *env, dsxp_response_t *out) {
    cmd_t c = { .kind = CMD_MOTION_RECENTER };
    submit(env, &c, out);
}

/* --------------------------------------------------------------- router */

void api_handle(const api_env_t *env, const char *method, const char *path,
                const char *query, dsxp_response_t *out) {
    memset(out, 0, sizeof *out);
    if (!method) method = "";
    if (!path) path = "";
    if (!query) query = "";
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");

    if (!strcmp(path, "/api/init")) {
        if (!get) { respond(out, 405, CT_TEXT, "method not allowed"); return; }
        api_init(env, out);
        return;
    }
    if (!strcmp(path, "/api/state")) {
        if (!get) { respond(out, 405, CT_TEXT, "method not allowed"); return; }
        api_state(env, out);
        return;
    }

    static const char *POST_ROUTES[] = {
        "/api/bind", "/api/axis", "/api/tune", "/api/light", "/api/trigger",
        "/api/triggersource", "/api/rumble", "/api/rumblesource", "/api/rumblepath",
        "/api/motion", "/api/motionrecenter",                 /* 9, 10 */
        "/api/calibrate",                                     /* 11 */
    };
    int route = -1;
    for (int i = 0; i < (int)(sizeof POST_ROUTES / sizeof POST_ROUTES[0]); i++)
        if (!strcmp(path, POST_ROUTES[i])) route = i;
    if (route < 0) { respond(out, 404, CT_TEXT, "no route"); return; }
    if (!post) { respond(out, 405, CT_TEXT, "method not allowed"); return; }

    /* routes that check the current state (sources, calibration) need a snapshot */
    dsxp_snap_t *snap = NULL;
    if (route == 4 || route == 6 || route == 9 || route == 11) {
        snap = calloc(1, sizeof *snap);
        if (!snap) { respond(out, 500, CT_TEXT, "out of memory"); return; }
        if (env->snapshot) env->snapshot(env->ctx, snap);
    }
    switch (route) {
    case 0: api_bind(env, query, out); break;
    case 1: api_axis(env, query, out); break;
    case 2: api_tune(env, query, out); break;
    case 3: api_light(env, query, out); break;
    case 4: api_trigger(env, snap, query, out); break;
    case 5: api_trigger_source(env, query, out); break;
    case 6: api_rumble(env, snap, query, out); break;
    case 7: api_rumble_source(env, query, out); break;
    case 8: api_rumblepath(env, query, out); break;
    case 9: api_motion(env, snap, query, out); break;
    case 10: api_motion_recenter(env, out); break;
    case 11: api_calibrate(env, snap, query, out); break;
    default: respond(out, 404, CT_TEXT, "no route"); break;
    }
    free(snap);
}
