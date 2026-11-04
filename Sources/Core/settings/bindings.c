/* Settings: profile defaults, and the importer for the old bindings.conf format
 * (the prototype and DSXP before the config files). Pure. */
#include "core_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Defaults transcribed from the author's own saved X-Plane profile
 * (Output/preferences/control profiles/final - Joysticks.prf, device slot 0 =
 * the Bluetooth DualSense). Slot 0 uses _joy_AXIS_use0.. and _joy_BUTN_use0..
 * directly (the 25-axis / 160-button per-device stride only shifts slot 1+).
 *
 * D-pad Up is deliberately left unbound: the profile leaves _joy_BUTN_use0 at
 * sim/none/none.
 *
 * Axis values on file are 2, 1, 3, 52, 74, 75. The enum was recovered from the
 * ordered string table inside the X-Plane binary, because the published docs
 * table stops at 66:
 *     1 = joy_use_ptch    3 = joy_use_hdng   52 = joy_use_commands
 *     2 = joy_use_roll   74 = joy_use_rudd_lft   75 = joy_use_rudd_rgt
 * So L2/R2 are differential RUDDER, not toe brakes. Axis 3 is joy_use_commands
 * with all four command slots cleared, i.e. unassigned.
 */
void load_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    struct { int i; const char *c; bool rep; } d[] = {
        { B_DP_UR,    "sim/general/hat_switch_up_right",       true  },
        { B_DP_R,     "sim/flight_controls/landing_gear_down", false },
        { B_DP_DR,    "sim/general/hat_switch_down_right",     true  },
        { B_DP_DOWN,  "sim/flight_controls/flaps_down",        false },
        { B_DP_DL,    "sim/general/hat_switch_down_left",      true  },
        { B_DP_L,     "sim/flight_controls/landing_gear_up",   false },
        { B_DP_UL,    "sim/general/hat_switch_up_left",        true  },
        { B_SQUARE,   "sim/general/left",                      true  },
        { B_CROSS,    "sim/engines/throttle_down",             true  },
        { B_CIRCLE,   "sim/general/right",                     true  },
        { B_TRIANGLE, "sim/engines/throttle_up",               true  },
        { B_L1,       "sim/general/zoom_out",                  true  },
        { B_R1,       "sim/general/zoom_in",                   true  },
        { B_L3,       "sim/flight_controls/pitch_trim_down",   true  },
        { B_R3,       "sim/flight_controls/pitch_trim_up",     true  },
    };
    for (size_t k = 0; k < sizeof d / sizeof d[0]; k++) {
        snprintf(s->bind[d[k].i].cmd, CMD_PATH_MAX, "%s", d[k].c);
        s->bind[d[k].i].repeat = d[k].rep;
    }
    s->axis[S_LX] = (axis_map_t){ AX_ROLL,   false };
    s->axis[S_LY] = (axis_map_t){ AX_PITCH,  false };
    s->axis[S_RX] = (axis_map_t){ AX_YAW,    false };
    s->axis[S_RY] = (axis_map_t){ AX_NONE,   false };  /* profile cleared it */
    s->axis[S_L2] = (axis_map_t){ AX_RUDD_L, false };
    s->axis[S_R2] = (axis_map_t){ AX_RUDD_R, false };

    /* Deadzone 0.06: measured drift at rest on the author's controller was about
     * 5% (rx +0.0315, ry -0.0472), and X-Plane's own null zone does not apply to
     * the override datarefs (observed), so nothing else would filter it. */
    s->tune = (tuning_t){ 0.06, 1.00, 0.5 };
    /* center lamp only is the DualSense's "player 1" indicator */
    s->light = (light_t){ 0, 90, 255, true, LP_SOLID, 0.5, 1.0, LM_MANUAL, 0x04 };
    s->fx_manual[0] = s->fx_manual[1] = (trig_manual_t){ TFX_OFF, 70, 160, 200, 40 };
    s->fxmode = FXM_MANUAL;
    s->rmode = RM_MANUAL;
    /* v2 produced no rumble at all on the author's controller; classic works */
    s->rumble_v2 = false;
    s->gesture = GESTURE_DEFAULTS;
    s->wind = WIND_DEFAULTS;
    s->rumble = RUMBLE_DEFAULTS;
    s->lsim = LIGHTSIM_DEFAULTS;
}

/* X-Plane command paths look like sim/flight_controls/flaps_up. Restricting the
 * alphabet keeps quotes, spaces and newlines out of the config file and JSON. */
bool cmd_valid(const char *cmd) {
    size_t n = 0;
    for (const char *p = cmd; *p; p++, n++) {
        char ch = *p;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '/' || ch == '.' ||
                  ch == '+' || ch == '-';
        if (!ok) return false;
    }
    return n >= 1 && n < CMD_PATH_MAX;
}

double clampd(double v, double lo, double hi, double dflt) {
    if (!isfinite(v)) return dflt;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int clampi(long v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int)v;
}

static void parse_line(settings_t *s, const char *line) {
    char what[32];
    if (sscanf(line, "%31s", what) != 1) return;

    if (!strcmp(what, "tune")) {
        char key[32]; double v;
        if (sscanf(line, "%*s %31s %lf", key, &v) == 2) {
            if (!strcmp(key, "deadzone"))     s->tune.deadzone = clampd(v, 0, 0.45, s->tune.deadzone);
            else if (!strcmp(key, "curve"))   s->tune.curve    = clampd(v, 0.4, 3.0, s->tune.curve);
            else if (!strcmp(key, "gesture")) s->tune.gesture  = clampd(v, 0, 1, s->tune.gesture);
        }
    } else if (!strcmp(what, "light")) {
        long r, g, b, on, pat = LP_SOLID, pl = 0x04, lm = LM_MANUAL;
        double sp = 0.5, bright = 1.0;
        /* extra fields were appended over time, so older lines still load */
        int got = sscanf(line, "%*s %ld %ld %ld %ld %ld %lf %ld %lf %ld",
                         &r, &g, &b, &on, &pat, &sp, &pl, &bright, &lm);
        if (got >= 4) {
            s->light.r = (unsigned char)clampi(r, 0, 255);
            s->light.g = (unsigned char)clampi(g, 0, 255);
            s->light.b = (unsigned char)clampi(b, 0, 255);
            s->light.on = on != 0;
        }
        if (got >= 5) s->light.pattern = (pat < 0 || pat >= LP_COUNT) ? LP_SOLID : (int)pat;
        if (got >= 6) s->light.speed   = clampd(sp, 0, 1, 0.5);
        if (got >= 7) s->light.player  = (unsigned char)(clampi(pl, 0, 255) & 0x1F);
        if (got >= 8) s->light.bright  = clampd(bright, 0, 1, 1.0);
        if (got >= 9) s->light.mode    = lm == LM_SIM ? LM_SIM : LM_MANUAL;
    } else if (!strcmp(what, "fxmode")) {
        char m[16] = "";
        if (sscanf(line, "%*s %15s", m) == 1)
            s->fxmode = strcmp(m, "wind") == 0 ? FXM_WIND : FXM_MANUAL;
    } else if (!strcmp(what, "rumblemode")) {
        char m[16] = "";
        if (sscanf(line, "%*s %15s", m) == 1)
            s->rmode = strcmp(m, "sim") == 0 ? RM_SIM : RM_MANUAL;
    } else if (!strcmp(what, "rumblepath")) {
        char m[16] = "";
        if (sscanf(line, "%*s %15s", m) == 1) s->rumble_v2 = strcmp(m, "v2") == 0;
    } else if (!strcmp(what, "trigger")) {
        char sd[4]; long m, st, en, sg, fq = -1;
        int got = sscanf(line, "%*s %3s %ld %ld %ld %ld %ld", sd, &m, &st, &en, &sg, &fq);
        if (got >= 5 && (!strcmp(sd, "l") || !strcmp(sd, "r"))) {
            int i = sd[0] == 'r' ? 1 : 0;
            trig_manual_t *f = &s->fx_manual[i];
            f->mode = (m < TFX_OFF || m > TFX_VIBRATE) ? TFX_OFF : (int)m;
            f->start = (unsigned char)clampi(st, 0, 255);
            f->strength = (unsigned char)clampi(sg, 0, 255);
            if (got == 6) {                          /* end and frequency stored apart */
                f->end = (unsigned char)clampi(en, 0, 255);
                f->freq = (unsigned char)clampi(fq, 1, 255);
            } else if (f->mode == TFX_VIBRATE) {     /* old files: vibrate's end WAS the frequency */
                f->freq = (unsigned char)clampi(en, 1, 255);
            } else {
                f->end = (unsigned char)clampi(en, 0, 255);
            }
        }
    } else if (!strcmp(what, "axis")) {
        char src[32], tgt[32]; int inv = 0;
        if (sscanf(line, "%*s %31s %31s %d", src, tgt, &inv) < 2) return;
        for (int i = 0; i < S_COUNT; i++) if (!strcmp(src, SRC_KEY[i]))
            for (int t = 0; t < AX_TARGET_COUNT; t++) if (!strcmp(tgt, AX_KEY[t]))
                s->axis[i] = (axis_map_t){ t, inv != 0 };
    } else if (!strcmp(what, "button")) {
        long slot = -1, rep = 0; char cmd[CMD_PATH_MAX] = "";
        if (sscanf(line, "%*s %ld %ld %255s", &slot, &rep, cmd) < 3) return;
        if (slot < 0 || slot >= B_COUNT || !cmd_valid(cmd)) return;
        snprintf(s->bind[slot].cmd, CMD_PATH_MAX, "%s", cmd);
        s->bind[slot].repeat = rep != 0;
    }
}

/* Parse bindings.conf text over whatever is already in *s (normally the
 * defaults). A file on disk is authoritative for buttons, so they start empty
 * rather than letting defaults leak into slots the user deliberately cleared. */
bool bindings_parse(settings_t *s, const char *text, size_t len) {
    if (!text) return false;
    memset(s->bind, 0, sizeof s->bind);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t n = i - start;
        if (i < len) i++;                          /* skip the newline */
        if (n == 0 || n >= 1024) continue;         /* empty or oversized: skip */
        char line[1024];
        memcpy(line, text + start, n);
        line[n] = 0;
        if (strlen(line) != n) continue;           /* embedded NUL: reject the line */
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (!n || line[0] == '#') continue;
        parse_line(s, line);
    }
    return true;
}
