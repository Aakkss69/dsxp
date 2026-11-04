/*
 * Minimal loopback-only HTTP server backing the mapper UI.
 *
 * Bound to 127.0.0.1 only - it can rewrite bindings and drive the sim, so it must
 * never be reachable off the machine.
 *
 * Routes
 *   GET  /                 web/index.html   (plus /app.css, /app.js)
 *   GET  /api/init         command catalogue + slot/source/target vocabulary
 *   GET  /api/state        live axes, buttons, gate, bindings, tuning
 *   POST /api/bind         ?i=<slot>&cmd=<path>&repeat=<0|1>   (empty cmd clears)
 *   POST /api/axis         ?k=<source>&t=<target>&inv=<0|1>
 *   POST /api/tune         ?deadzone=<f>&curve=<f>&gesture=<f>
 *   POST /api/light        ?r=&g=&b=&on=<0|1>&pattern=<0-5>&speed=<f>&player=<0-31>&bright=<f>&lmode=<manual|sim>
 *   POST /api/trigger      ?side=<l|r>&mode=<0-3>&start=&end=&strength=
 *   POST /api/fxmode       ?mode=<manual|wind>
 *   POST /api/rumble       ?left=&right=      (manual mode only)
 *   POST /api/rumblemode   ?mode=<manual|sim>
 *   POST /api/quit         shut the bridge down cleanly
 */
#include "dsxp.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

/* ------------------------------------------------------------- utilities */

/*
 * Bounded append.
 *
 * The obvious idiom - o += snprintf(buf + o, cap - o, ...) - is a trap:
 * snprintf returns what it WOULD have written, so once output is truncated o
 * runs past cap, and the next call computes cap - o as a size_t underflow. That
 * hands snprintf a length near SIZE_MAX and it happily writes off the end of
 * the buffer. Clamping o here makes truncation lose data instead of corrupting
 * the heap.
 */
static void jput(char *b, size_t cap, size_t *o, const char *fmt, ...) {
    if (cap == 0 || *o >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + *o, cap - *o, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *o += (size_t)n;
    if (*o > cap - 1) *o = cap - 1;          /* truncated: stay inside */
}

static void json_escape(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = s; *p && o + 8 < n; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = *p; }
        else if ((unsigned char)*p < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", *p);
        else out[o++] = *p;
    }
    out[o] = 0;
}

static void url_decode(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (*p == '+') out[o++] = ' ';
        else out[o++] = *p;
    }
    out[o] = 0;
}

static const char *qs_get(const char *qs, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "%s=", key);
    const char *p = strstr(qs, pat);
    /* only accept a match at the start or right after '&', so "inv=" cannot be
     * satisfied by the tail of some other parameter name */
    while (p && p != qs && p[-1] != '&') p = strstr(p + 1, pat);
    if (!p) { out[0] = 0; return NULL; }
    p += strlen(pat);
    const char *e = strchr(p, '&');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char raw[1024];
    if (len >= sizeof raw) len = sizeof raw - 1;
    memcpy(raw, p, len); raw[len] = 0;
    url_decode(raw, out, n);
    return out;
}

/* send() on a socket may write less than asked. app.js is ~29 KB, so a short
 * write would silently truncate it and hand the browser broken JavaScript. */
static bool send_all(int c, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static void http_send(int c, const char *status, const char *ctype,
                      const char *body, size_t len) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, ctype, len);
    if (hl < 0) return;
    if (!send_all(c, hdr, (size_t)hl)) return;
    if (body && len) send_all(c, body, len);
}

static void send_file(int c, const char *name, const char *ctype) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/web/%s", g_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        char msg[PATH_MAX + 32];
        int n = snprintf(msg, sizeof msg, "not found: %s", p);
        http_send(c, "404 Not Found", "text/plain", msg, (size_t)n);
        return;
    }
    struct stat st;
    if (stat(p, &st) != 0) { fclose(f); return; }
    char *b = malloc((size_t)st.st_size + 1);
    if (!b) { fclose(f); return; }
    size_t n = fread(b, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {          /* partial read means a partial page */
        fprintf(stderr, "[dsxp] short read on %s (%zu of %lld)\n",
                p, n, (long long)st.st_size);
    }
    http_send(c, "200 OK", ctype, b, n);
    free(b);
}

/* ------------------------------------------------------------- endpoints */

static void api_init(int c) {
    size_t cap = (size_t)g_ncmds * 600 + 65536;
    char *b = malloc(cap);
    if (!b) { http_send(c, "500 Error", "text/plain", "oom", 3); return; }
    size_t o = 0;
    jput(b, cap, &o, "{\"commands\":[");
    char esc[CMD_DESC_MAX * 2];
    for (int i = 0; i < g_ncmds; i++) {
        json_escape(g_cmds[i].desc, esc, sizeof esc);
        jput(b, cap, &o, "%s{\"c\":\"%s\",\"d\":\"%s\"}",
                              i ? "," : "", g_cmds[i].path, esc);
    }
    jput(b, cap, &o, "],\"slots\":[");
    for (int i = 0; i < B_COUNT; i++)
        jput(b, cap, &o, "%s{\"i\":%d,\"n\":\"%s\"}",
                              i ? "," : "", i, BTN_NAME[i]);
    jput(b, cap, &o, "],\"sources\":[");
    for (int i = 0; i < S_COUNT; i++)
        jput(b, cap, &o, "%s{\"k\":\"%s\",\"n\":\"%s\"}",
                              i ? "," : "", SRC_KEY[i], SRC_LABEL[i]);
    jput(b, cap, &o, "],\"targets\":[");
    for (int i = 0; i < AX_TARGET_COUNT; i++)
        jput(b, cap, &o, "%s{\"k\":\"%s\",\"n\":\"%s\"}",
                              i ? "," : "", AX_KEY[i], AX_LABEL[i]);
    jput(b, cap, &o, "]}");
    http_send(c, "200 OK", "application/json", b, o);
    free(b);
}

static void api_state(int c) {
    char b[16384];
    size_t o = 0;
    pthread_mutex_lock(&g_lock);
    jput(b, sizeof b, &o,
        "{\"connected\":%s,\"reports\":%ld,\"total\":%ld,"
        "\"tune\":{\"deadzone\":%.4f,\"curve\":%.4f,\"gesture\":%.3f},"
        "\"light\":{\"r\":%d,\"g\":%d,\"b\":%d,\"on\":%d,"
        "\"pattern\":%d,\"speed\":%.3f,\"player\":%d,"
        "\"bright\":%.3f,\"lmode\":\"%s\",\"lnote\":\"%s\"},"
        "\"fx\":[{\"mode\":%d,\"start\":%d,\"end\":%d,\"strength\":%d},"
        "{\"mode\":%d,\"start\":%d,\"end\":%d,\"strength\":%d}],"
        "\"fxmode\":\"%s\","
        "\"wx\":{\"live\":%d,\"ias\":%.1f,\"wind\":%.1f,\"dir\":%.0f,"
        "\"hdg\":%.0f,\"turb\":%.2f,\"xwind\":%.1f,\"load\":%.3f,\"note\":\"%s\","
        "\"stall_l\":%.3f,\"stall_r\":%.3f,\"stall\":%.3f,\"gn\":%.2f,\"crashed\":%d},"
        "\"rmode\":\"%s\",\"rnote\":\"%s\","
        "\"rumble\":{\"left\":%d,\"right\":%d,\"v2\":%d},"
        "\"gate\":{\"bt\":%d,\"usb\":%d,\"xp\":%d,\"engaged\":%d},\"axes\":[",
        g_have_report ? "true" : "false", g_reports, g_reports_total,
        g_tune.deadzone, g_tune.curve, g_tune.gesture,
        g_light.r, g_light.g, g_light.b, g_light.on ? 1 : 0,
        g_light.pattern, g_light.speed, g_light.player,
        g_light.bright, g_light.mode == LM_SIM ? "sim" : "manual", g_lnote,
        g_fx[0].mode, g_fx[0].start, g_fx[0].end, g_fx[0].strength,
        g_fx[1].mode, g_fx[1].start, g_fx[1].end, g_fx[1].strength,
        g_fxmode == FXM_WIND ? "wind" : "manual",
        g_wx.live ? 1 : 0, g_wx.ias, g_wx.wind_ms, g_wx.wind_dir,
        g_wx.heading, g_wx.turb, g_wx.crosswind, g_wx.load, g_wx.note,
        g_wx.stall_l, g_wx.stall_r, g_wx.stall_ratio, g_wx.gn, g_wx.crashed ? 1 : 0,
        g_rmode == RM_SIM ? "sim" : "manual", g_rnote,
        g_rumble.left, g_rumble.right, g_rumble_v2 ? 1 : 0,
        g_bt_present, g_usb_present, g_xp_running, g_engaged);
    for (int i = 0; i < S_COUNT; i++)
        jput(b, sizeof b, &o, "%s%.4f", i ? "," : "", g_state.val[i]);
    jput(b, sizeof b, &o, "],\"raw\":[");
    for (int i = 0; i < S_COUNT; i++)
        jput(b, sizeof b, &o, "%s%.4f", i ? "," : "", g_state.raw01[i]);
    jput(b, sizeof b, &o, "],\"btn\":[");
    for (int i = 0; i < B_COUNT; i++)
        jput(b, sizeof b, &o, "%s%d", i ? "," : "", g_state.btn[i] ? 1 : 0);
    jput(b, sizeof b, &o, "],\"bind\":[");
    for (int i = 0; i < B_COUNT; i++)
        jput(b, sizeof b, &o, "%s{\"c\":\"%s\",\"r\":%d}",
                              i ? "," : "", g_bind[i].cmd, g_bind[i].repeat ? 1 : 0);
    jput(b, sizeof b, &o, "],\"axismap\":[");
    for (int i = 0; i < S_COUNT; i++)
        jput(b, sizeof b, &o, "%s{\"t\":\"%s\",\"inv\":%d}",
                              i ? "," : "", AX_KEY[g_axis[i].target], g_axis[i].invert ? 1 : 0);
    jput(b, sizeof b, &o, "],\"touch\":[");
    for (int i = 0; i < 2; i++)
        jput(b, sizeof b, &o, "%s{\"on\":%d,\"x\":%.4f,\"y\":%.4f}",
                              i ? "," : "", g_state.touch[i].active ? 1 : 0,
                              g_state.touch[i].x, g_state.touch[i].y);
    jput(b, sizeof b, &o, "]}");
    pthread_mutex_unlock(&g_lock);
    http_send(c, "200 OK", "application/json", b, o);
}

static void api_bind(int c, const char *qs) {
    char v[1024];
    int slot = -1, rep = 0;
    if (qs_get(qs, "i", v, sizeof v)) slot = atoi(v);
    if (qs_get(qs, "repeat", v, sizeof v)) rep = atoi(v);
    char cmd[CMD_PATH_MAX] = "";
    qs_get(qs, "cmd", cmd, sizeof cmd);
    if (slot < 0 || slot >= B_COUNT) {
        http_send(c, "400 Bad Request", "text/plain", "bad slot", 8); return;
    }
    pthread_mutex_lock(&g_lock);
    strncpy(g_bind[slot].cmd, cmd, CMD_PATH_MAX - 1);
    g_bind[slot].cmd[CMD_PATH_MAX - 1] = 0;
    g_bind[slot].repeat = rep != 0;
    bindings_save();
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[dsxp] bind %s -> %s%s\n", BTN_NAME[slot],
            cmd[0] ? cmd : "(cleared)", rep ? " [hold]" : "");
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_axis(int c, const char *qs) {
    char k[32] = "", t[32] = "", v[32] = "";
    qs_get(qs, "k", k, sizeof k);
    qs_get(qs, "t", t, sizeof t);
    qs_get(qs, "inv", v, sizeof v);
    int si = -1, ti = -1;
    for (int i = 0; i < S_COUNT; i++) if (!strcmp(k, SRC_KEY[i])) si = i;
    for (int i = 0; i < AX_TARGET_COUNT; i++) if (!strcmp(t, AX_KEY[i])) ti = i;
    if (si < 0 || ti < 0) { http_send(c, "400 Bad Request", "text/plain", "bad axis", 8); return; }
    pthread_mutex_lock(&g_lock);
    g_axis[si].target = ti;
    g_axis[si].invert = atoi(v) != 0;
    bindings_save();
    bool was = g_engaged;
    pthread_mutex_unlock(&g_lock);
    /* re-assert overrides so a newly assigned axis takes effect without a restart */
    if (was) { g_engaged = false; xp_engage(); }
    fprintf(stderr, "[dsxp] axis %s -> %s%s\n", SRC_LABEL[si], AX_LABEL[ti],
            g_axis[si].invert ? " (inverted)" : "");
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_tune(int c, const char *qs) {
    char v[64];
    pthread_mutex_lock(&g_lock);
    if (qs_get(qs, "deadzone", v, sizeof v)) {
        double d = atof(v);
        if (d < 0)    d = 0;
        if (d > 0.45) d = 0.45;   /* beyond this the stick has almost no travel left */
        g_tune.deadzone = d;
    }
    if (qs_get(qs, "gesture", v, sizeof v)) {
        double gv = atof(v);
        if (gv < 0) gv = 0;
        if (gv > 1) gv = 1;
        g_tune.gesture = gv;
    }
    if (qs_get(qs, "curve", v, sizeof v)) {
        double cv = atof(v);
        if (cv < 0.4) cv = 0.4;   /* <1 sharpens, >1 softens around centre */
        if (cv > 3.0) cv = 3.0;
        g_tune.curve = cv;
    }
    bindings_save();
    double d = g_tune.deadzone, cv = g_tune.curve;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[dsxp] tune deadzone=%.3f curve=%.2f\n", d, cv);
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_light(int c, const char *qs) {
    char v[32];
    pthread_mutex_lock(&g_lock);
    if (qs_get(qs, "r", v, sizeof v)) g_light.r = (unsigned char)atoi(v);
    if (qs_get(qs, "g", v, sizeof v)) g_light.g = (unsigned char)atoi(v);
    if (qs_get(qs, "b", v, sizeof v)) g_light.b = (unsigned char)atoi(v);
    if (qs_get(qs, "on", v, sizeof v)) g_light.on = atoi(v) != 0;
    if (qs_get(qs, "pattern", v, sizeof v)) {
        int p = atoi(v);
        g_light.pattern = (p < 0 || p >= LP_COUNT) ? LP_SOLID : p;
    }
    if (qs_get(qs, "speed", v, sizeof v)) {
        double sp = atof(v);
        g_light.speed = sp < 0 ? 0 : (sp > 1 ? 1 : sp);
    }
    /* five lamps, so only the low five bits mean anything */
    if (qs_get(qs, "player", v, sizeof v)) g_light.player = (unsigned char)(atoi(v) & 0x1F);
    if (qs_get(qs, "bright", v, sizeof v)) {
        double bv = atof(v);
        g_light.bright = bv < 0 ? 0 : (bv > 1 ? 1 : bv);
    }
    if (qs_get(qs, "lmode", v, sizeof v))
        g_light.mode = strcmp(v, "sim") == 0 ? LM_SIM : LM_MANUAL;
    bindings_save();
    pthread_mutex_unlock(&g_lock);
    out_mark();
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* Adaptive trigger effect. Parameters mean different things per mode:
 *   RESIST  start = where resistance begins, strength = how hard
 *   WEAPON  start/end = the resisting band, strength = break-through force
 *   VIBRATE start = threshold, strength = amplitude, end = frequency */
static void api_trigger(int c, const char *qs) {
    char v[32];
    int side = -1;
    if (qs_get(qs, "side", v, sizeof v)) side = (v[0] == 'r') ? 1 : (v[0] == 'l') ? 0 : -1;
    if (side < 0) { http_send(c, "400 Bad Request", "text/plain", "bad side", 8); return; }
    pthread_mutex_lock(&g_lock);
    if (g_fxmode == FXM_WIND) {          /* wind mode owns the effects */
        pthread_mutex_unlock(&g_lock);
        http_send(c, "409 Conflict", "text/plain", "wind mode", 9);
        return;
    }
    trigger_fx_t *f = &g_fx[side];
    if (qs_get(qs, "mode", v, sizeof v)) {
        int m = atoi(v);
        f->mode = (m < 0 || m > TFX_VIBRATE) ? TFX_OFF : m;
    }
    if (qs_get(qs, "start", v, sizeof v))    f->start    = (unsigned char)atoi(v);
    if (qs_get(qs, "end", v, sizeof v))      f->end      = (unsigned char)atoi(v);
    if (qs_get(qs, "strength", v, sizeof v)) f->strength = (unsigned char)atoi(v);
    bindings_save();
    int m = f->mode, st = f->start, en = f->end, sg = f->strength;
    pthread_mutex_unlock(&g_lock);
    out_mark();
    fprintf(stderr, "[dsxp] trigger %s mode=%d start=%d end=%d strength=%d\n",
            side ? "R2" : "L2", m, st, en, sg);
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* Manual and wind are mutually exclusive: wind mode owns g_fx while it runs, so
 * leaving it restores whatever was hand-set last. */
static void api_fxmode(int c, const char *qs) {
    char v[32];
    if (!qs_get(qs, "mode", v, sizeof v)) {
        http_send(c, "400 Bad Request", "text/plain", "no mode", 7); return;
    }
    static trigger_fx_t saved[2];
    static bool have_saved = false;
    pthread_mutex_lock(&g_lock);
    fxmode_t want = strcmp(v, "wind") == 0 ? FXM_WIND : FXM_MANUAL;
    if (want != g_fxmode) {
        if (want == FXM_WIND) {
            saved[0] = g_fx[0]; saved[1] = g_fx[1];
            have_saved = true;
        } else if (have_saved) {
            g_fx[0] = saved[0]; g_fx[1] = saved[1];
        }
        g_fxmode = want;
    }
    bindings_save();
    pthread_mutex_unlock(&g_lock);
    out_mark();
    fprintf(stderr, "[dsxp] trigger source -> %s\n", want == FXM_WIND ? "wind" : "manual");
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_rumble(int c, const char *qs) {
    char v[32];
    pthread_mutex_lock(&g_lock);
    if (g_rmode == RM_SIM) {            /* sim mode owns the motors */
        pthread_mutex_unlock(&g_lock);
        http_send(c, "409 Conflict", "text/plain", "sim mode", 8);
        return;
    }
    if (qs_get(qs, "left", v, sizeof v))  g_rumble.left  = (unsigned char)atoi(v);
    if (qs_get(qs, "right", v, sizeof v)) g_rumble.right = (unsigned char)atoi(v);
    snprintf(g_rnote, sizeof g_rnote, "manual %d / %d", g_rumble.left, g_rumble.right);
    bindings_save();
    pthread_mutex_unlock(&g_lock);
    out_mark();
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_rumblemode(int c, const char *qs) {
    char v[32];
    if (!qs_get(qs, "mode", v, sizeof v)) {
        http_send(c, "400 Bad Request", "text/plain", "no mode", 7); return;
    }
    pthread_mutex_lock(&g_lock);
    rmode_t want = strcmp(v, "sim") == 0 ? RM_SIM : RM_MANUAL;
    if (want != g_rmode) {
        g_rmode = want;
        /* never leave the motors spinning when handing control over */
        g_rumble.left = g_rumble.right = 0;
    }
    bindings_save();
    pthread_mutex_unlock(&g_lock);
    out_mark();
    fprintf(stderr, "[dsxp] rumble source -> %s\n", want == RM_SIM ? "sim" : "manual");
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void api_rumblepath(int c, const char *qs) {
    char v[16];
    if (qs_get(qs, "v2", v, sizeof v)) {
        pthread_mutex_lock(&g_lock);
        g_rumble_v2 = atoi(v) != 0;
        bindings_save();
        pthread_mutex_unlock(&g_lock);
        out_mark();
        fprintf(stderr, "[dsxp] rumble path -> %s\n", g_rumble_v2 ? "v2" : "classic");
    }
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* Reply first, then ask the run loop to stop - otherwise the browser sees the
 * connection drop and reports an error for a perfectly clean shutdown. */
static void api_quit(int c) {
    http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
    close(c);
    fprintf(stderr, "[dsxp] quit requested from the UI\n");
    g_stop = 1;
}

/* ---------------------------------------------------------------- server */

void *httpd_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(UI_PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);   /* loopback only, never public */
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "[dsxp] UI port %d unavailable: %s\n", UI_PORT, strerror(errno));
        close(s);
        return NULL;
    }
    listen(s, 16);
    fprintf(stderr, "[dsxp] mapper UI at http://127.0.0.1:%d\n", UI_PORT);

    while (!g_stop) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        char req[4096];
        ssize_t n = recv(c, req, sizeof req - 1, 0);
        if (n <= 0) { close(c); continue; }
        req[n] = 0;
        char method[8] = "", url[2048] = "";
        sscanf(req, "%7s %2047s", method, url);
        char *qs = strchr(url, '?');
        if (qs) *qs++ = 0; else qs = (char *)"";

        if      (!strcmp(url, "/") || !strcmp(url, "/index.html"))
                                             send_file(c, "index.html", "text/html; charset=utf-8");
        else if (!strcmp(url, "/app.css"))   send_file(c, "app.css", "text/css; charset=utf-8");
        else if (!strcmp(url, "/app.js"))    send_file(c, "app.js", "text/javascript; charset=utf-8");
        else if (!strcmp(url, "/api/init"))  api_init(c);
        else if (!strcmp(url, "/api/state")) api_state(c);
        else if (!strcmp(url, "/api/bind"))  api_bind(c, qs);
        else if (!strcmp(url, "/api/axis"))  api_axis(c, qs);
        else if (!strcmp(url, "/api/tune"))  api_tune(c, qs);
        else if (!strcmp(url, "/api/light")) api_light(c, qs);
        else if (!strcmp(url, "/api/trigger")) api_trigger(c, qs);
        else if (!strcmp(url, "/api/fxmode")) api_fxmode(c, qs);
        else if (!strcmp(url, "/api/rumble")) api_rumble(c, qs);
        else if (!strcmp(url, "/api/rumblemode")) api_rumblemode(c, qs);
        else if (!strcmp(url, "/api/rumblepath")) api_rumblepath(c, qs);
        else if (!strcmp(url, "/api/quit")) { api_quit(c); break; }
        else http_send(c, "404 Not Found", "text/plain", "no route", 8);
        close(c);
    }
    close(s);
    return NULL;
}
