/* Binding defaults, persistence, and the X-Plane command catalogue. */
#include "dsxp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void bindings_path(char *out, size_t n) {
    snprintf(out, n, "%s/bindings.conf", g_dir);
}

/*
 * Defaults transcribed VERBATIM from the user's own saved profile:
 *   X-Plane 12/Output/preferences/control profiles/final - Joysticks.prf
 * device slot 0 - the one whose _joy_location0 reads
 *   Bluetooth_054c_0ce6_<controller-bt-address>_<id>_0001_0005
 * i.e. the Bluetooth DualSense's own on-file mapping, which is the device this
 * bridge drives. Slot 0 uses _joy_AXIS_use0.. and _joy_BUTN_use0.. directly (the
 * 25-axis / 160-button per-device stride only shifts slot 1 and above).
 *
 * Nothing here is merged or invented. D-pad Up is deliberately left unbound
 * because the profile leaves _joy_BUTN_use0 at sim/none/none.
 *
 * Axis values on file are 2, 1, 3, 52, 74, 75. The enum was recovered from the
 * ordered string table inside the X-Plane binary itself, because the published
 * docs table stops at 66:
 *     1 = joy_use_ptch    3 = joy_use_hdng   52 = joy_use_commands
 *     2 = joy_use_roll   74 = joy_use_rudd_lft   75 = joy_use_rudd_rgt
 * So L2/R2 are differential RUDDER, not toe brakes. Axis 3 is joy_use_commands
 * with all four _joy_AXIS_cmnd12..15 slots at sim/none/none, i.e. cleared.
 */
static void load_defaults(void) {
    memset(g_bind, 0, sizeof g_bind);
    struct { int i; const char *c; bool rep; } d[] = {
        /* _joy_BUTN_use0 is sim/none/none on file - D-pad Up stays unbound */
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
        strncpy(g_bind[d[k].i].cmd, d[k].c, CMD_PATH_MAX - 1);
        g_bind[d[k].i].repeat = d[k].rep;
    }
    g_axis[S_LX] = (axis_map_t){ AX_ROLL,   false };
    g_axis[S_LY] = (axis_map_t){ AX_PITCH,  false };
    g_axis[S_RX] = (axis_map_t){ AX_YAW,    false };
    g_axis[S_RY] = (axis_map_t){ AX_NONE,   false };  /* profile cleared it */
    g_axis[S_L2] = (axis_map_t){ AX_RUDD_L, false };
    g_axis[S_R2] = (axis_map_t){ AX_RUDD_R, false };
}

void bindings_save(void) {
    char p[PATH_MAX]; bindings_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "[dsxp] cannot write %s: %s\n", p, strerror(errno)); return; }
    fprintf(f, "# dsxp bindings - rewritten by the mapper UI\n");
    fprintf(f, "tune deadzone %.4f\n", g_tune.deadzone);
    fprintf(f, "tune curve %.4f\n",    g_tune.curve);
    fprintf(f, "tune gesture %.4f\n",  g_tune.gesture);
    fprintf(f, "light %d %d %d %d %d %.3f %d %.3f %d\n", g_light.r, g_light.g, g_light.b,
            g_light.on ? 1 : 0, g_light.pattern, g_light.speed, g_light.player,
            g_light.bright, g_light.mode);
    fprintf(f, "fxmode %s\n", g_fxmode == FXM_WIND ? "wind" : "manual");
    fprintf(f, "rumblemode %s\n", g_rmode == RM_SIM ? "sim" : "manual");
    fprintf(f, "rumblepath %s\n", g_rumble_v2 ? "v2" : "classic");
    for (int i = 0; i < 2; i++)
        fprintf(f, "trigger %c %d %d %d %d\n", i ? 'r' : 'l',
                g_fx[i].mode, g_fx[i].start, g_fx[i].end, g_fx[i].strength);
    fprintf(f, "# axis <source> <target> <invert>\n");
    for (int i = 0; i < S_COUNT; i++)
        fprintf(f, "axis %s %s %d\n", SRC_KEY[i], AX_KEY[g_axis[i].target],
                g_axis[i].invert ? 1 : 0);
    fprintf(f, "# button <slot> <repeat> <command>\n");
    for (int i = 0; i < B_COUNT; i++)
        if (g_bind[i].cmd[0])
            fprintf(f, "button %d %d %s\n", i, g_bind[i].repeat ? 1 : 0, g_bind[i].cmd);
    fclose(f);
}

void bindings_load(void) {
    load_defaults();
    char p[PATH_MAX]; bindings_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {
        /* materialise the defaults so the file is there to inspect and hand-edit */
        bindings_save();
        fprintf(stderr, "[dsxp] no bindings.conf - wrote profile defaults to %s\n", p);
        return;
    }
    /* A config on disk is authoritative, so start from empty buttons rather than
     * letting defaults leak through for slots the user deliberately cleared. */
    memset(g_bind, 0, sizeof g_bind);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char what[32];
        if (sscanf(line, "%31s", what) != 1) continue;
        if (!strcmp(what, "tune")) {
            char key[32]; double v;
            if (sscanf(line, "%*s %31s %lf", key, &v) == 2) {
                if (!strcmp(key, "deadzone")) g_tune.deadzone = v;
                else if (!strcmp(key, "curve")) g_tune.curve = v;
                else if (!strcmp(key, "gesture")) g_tune.gesture = v;
            }
        } else if (!strcmp(what, "light")) {
            int r, g, b, on, pat = LP_SOLID, pl = 0x04;
            double sp = 0.5;
            double bright = 1.0;
            int lm = LM_MANUAL;
            /* extra fields are appended, so older config lines still load */
            int got = sscanf(line, "%*s %d %d %d %d %d %lf %d %lf %d",
                             &r, &g, &b, &on, &pat, &sp, &pl, &bright, &lm);
            if (got >= 4) {
                g_light.r = (unsigned char)r;
                g_light.g = (unsigned char)g;
                g_light.b = (unsigned char)b;
                g_light.on = on != 0;
            }
            if (got >= 5) g_light.pattern = (pat < 0 || pat >= LP_COUNT) ? LP_SOLID : pat;
            if (got >= 6) g_light.speed   = sp < 0 ? 0 : (sp > 1 ? 1 : sp);
            if (got >= 7) g_light.player  = (unsigned char)(pl & 0x1F);
            if (got >= 8) g_light.bright  = bright < 0 ? 0 : (bright > 1 ? 1 : bright);
            if (got >= 9) g_light.mode    = lm == LM_SIM ? LM_SIM : LM_MANUAL;
        } else if (!strcmp(what, "fxmode")) {
            char m[16] = "";
            if (sscanf(line, "%*s %15s", m) == 1)
                g_fxmode = strcmp(m, "wind") == 0 ? FXM_WIND : FXM_MANUAL;
        } else if (!strcmp(what, "rumblemode")) {
            char m[16] = "";
            if (sscanf(line, "%*s %15s", m) == 1)
                g_rmode = strcmp(m, "sim") == 0 ? RM_SIM : RM_MANUAL;
        } else if (!strcmp(what, "rumblepath")) {
            char m[16] = "";
            if (sscanf(line, "%*s %15s", m) == 1) g_rumble_v2 = strcmp(m, "v2") == 0;
        } else if (!strcmp(what, "trigger")) {
            char sd; int m, st, en, sg;
            if (sscanf(line, "%*s %c %d %d %d %d", &sd, &m, &st, &en, &sg) == 5) {
                int i = (sd == 'r') ? 1 : 0;
                g_fx[i] = (trigger_fx_t){ m, (unsigned char)st,
                                          (unsigned char)en, (unsigned char)sg };
            }
        } else if (!strcmp(what, "axis")) {
            char src[32], tgt[32]; int inv = 0;
            if (sscanf(line, "%*s %31s %31s %d", src, tgt, &inv) < 2) continue;
            for (int i = 0; i < S_COUNT; i++) if (!strcmp(src, SRC_KEY[i]))
                for (int t = 0; t < AX_TARGET_COUNT; t++) if (!strcmp(tgt, AX_KEY[t]))
                    g_axis[i] = (axis_map_t){ t, inv != 0 };
        } else if (!strcmp(what, "button")) {
            int slot = -1, rep = 0; char cmd[CMD_PATH_MAX] = "";
            if (sscanf(line, "%*s %d %d %255s", &slot, &rep, cmd) < 3) continue;
            if (slot < 0 || slot >= B_COUNT) continue;
            strncpy(g_bind[slot].cmd, cmd, CMD_PATH_MAX - 1);
            g_bind[slot].repeat = rep != 0;
        }
    }
    fclose(f);
    fprintf(stderr, "[dsxp] loaded bindings from %s\n", p);
}

/*
 * X-Plane ships every command with a human-readable description in
 * Resources/plugins/Commands.txt, formatted as <path> space-padded then <text>.
 * That file is what makes the mapper searchable in plain English.
 */
void commands_load(void) {
    const char *candidates[] = {
        /* (a hard-coded absolute install path was removed on import) */
        "Commands.txt"
    };
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0] && !f; i++)
        f = fopen(candidates[i], "r");
    if (!f) { fprintf(stderr, "[dsxp] Commands.txt not found - UI list will be empty\n"); return; }

    g_cmds = calloc(MAX_CMDS, sizeof(xcmd_t));
    if (!g_cmds) { fclose(f); return; }
    char line[1024];
    while (fgets(line, sizeof line, f) && g_ncmds < MAX_CMDS) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!line[0]) continue;
        char *sp = strstr(line, "  ");
        if (!sp) {
            strncpy(g_cmds[g_ncmds].path, line, CMD_PATH_MAX - 1);
            g_cmds[g_ncmds].desc[0] = 0;
        } else {
            size_t plen = (size_t)(sp - line);
            if (plen >= CMD_PATH_MAX) plen = CMD_PATH_MAX - 1;
            memcpy(g_cmds[g_ncmds].path, line, plen);
            g_cmds[g_ncmds].path[plen] = 0;
            while (*sp == ' ') sp++;
            strncpy(g_cmds[g_ncmds].desc, sp, CMD_DESC_MAX - 1);
        }
        g_ncmds++;
    }
    fclose(f);
    fprintf(stderr, "[dsxp] loaded %d X-Plane commands\n", g_ncmds);
}
