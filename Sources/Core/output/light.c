/* Light bar: manual color/patterns, or an annunciator set by the simulator. */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>

void hsv(double h, double s, double v, unsigned char *R, unsigned char *G, unsigned char *B) {
    h = fmod(h, 1.0);
    if (h < 0) h += 1.0;
    h *= 6.0;
    int i = (int)h;
    double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r, g, b;
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default:r = v; g = p; b = q; break;
    }
    *R = (unsigned char)(r * 255); *G = (unsigned char)(g * 255); *B = (unsigned char)(b * 255);
}

/* Sim mode: the bar becomes an annunciator. Ordered by urgency, because only one
 * thing can be shown at a time and the worst news should win. */
void light_sim(const wx_t *wx, int *pat, unsigned char *r, unsigned char *g,
               unsigned char *b, double *rate, char *note, size_t note_n,
               const light_sim_cfg_t *cfg) {
    if (!cfg) cfg = &LIGHT_SIM_DEFAULTS;
    if (!wx->live) {                         /* no telemetry: dim amber standby */
        *pat = LP_BREATHE; *r = 120; *g = 70; *b = 0; *rate = 0.3;
        snprintf(note, note_n, "Waiting for X-Plane");
        return;
    }
    if (wx->crashed) {
        *pat = LP_STROBE;  *r = 255; *g = 0; *b = 0; *rate = 3.0;
        snprintf(note, note_n, "Crashed");
    } else if (wx->stall_ratio > cfg->stall_alarm) {
        *pat = LP_PULSE;   *r = 255; *g = 0; *b = 0; *rate = 2.5;
        snprintf(note, note_n, "Stall %.0f%%", wx->stall_ratio * 100);
    } else if (wx->stall_ratio > cfg->stall_caution) {
        *pat = LP_BREATHE; *r = 255; *g = 120; *b = 0; *rate = 1.5;
        snprintf(note, note_n, "Stall warning");
    } else if (fabs(wx->gn) > cfg->g_caution) {
        *pat = LP_SOLID;   *r = 255; *g = 150; *b = 0; *rate = 1.0;
        snprintf(note, note_n, "Load %.1f g", wx->gn);
    } else if (wx->turb > cfg->turbulence_caution) {
        *pat = LP_BREATHE; *r = 0; *g = 200; *b = 220; *rate = 1.2;
        snprintf(note, note_n, "Turbulence %.1f", wx->turb);
    } else {
        *pat = LP_SOLID;   *r = 0; *g = 190; *b = 90; *rate = 1.0;
        snprintf(note, note_n, "Clean");
    }
}

/* Recompute the emitted color. Patterns refresh at 20 Hz - the bar cannot
 * resolve faster and it keeps the Bluetooth link free for input. */
bool light_tick(core_t *c, double now) {
    const light_t *L = &c->s.light;
    int pattern = L->pattern;
    unsigned char br = L->r, bg = L->g, bb = L->b;
    double rate = 0.25 + 2.25 * L->speed;
    double k0 = L->bright;

    /* sim mode overrides color, pattern and rate; manual mode uses the picker */
    if (L->source == LIGHT_SRC_SIM) light_sim(&c->wx, &pattern, &br, &bg, &bb, &rate,
                                     c->light_note, sizeof c->light_note, &c->s.light_sim);
    else snprintf(c->light_note, sizeof c->light_note, "Manual");

    if (!isfinite(k0) || k0 < 0) k0 = 0;
    if (k0 > 1) k0 = 1;

    unsigned char r = br, g = bg, b = bb;

    if (pattern != LP_SOLID) {
        if (now - c->light_last < 1.0 / 20.0) return false;
        c->light_last = now;
        double ph = fmod(now * rate, 1.0);
        switch (pattern) {
        case LP_BREATHE: {                          /* smooth sine fade */
            double k = 0.12 + 0.88 * (0.5 - 0.5 * cos(ph * 2 * M_PI));
            r = (unsigned char)(br * k); g = (unsigned char)(bg * k); b = (unsigned char)(bb * k);
            break;
        }
        case LP_PULSE: {                            /* quick rise, slow decay */
            double k = ph < 0.15 ? ph / 0.15 : 1.0 - (ph - 0.15) / 0.85;
            r = (unsigned char)(br * k); g = (unsigned char)(bg * k); b = (unsigned char)(bb * k);
            break;
        }
        case LP_STROBE:                             /* hard on/off */
            if (ph > 0.25) { r = g = b = 0; }
            break;
        case LP_RAINBOW:
            hsv(ph, 1.0, 1.0, &r, &g, &b);
            break;
        case LP_ALERT:                              /* base color vs amber */
            if (ph > 0.5) { r = 255; g = 120; b = 0; }
            break;
        default: break;
        }
    }

    r = (unsigned char)(r * k0);
    g = (unsigned char)(g * k0);
    b = (unsigned char)(b * k0);
    if (r != c->eff_r || g != c->eff_g || b != c->eff_b) {
        c->eff_r = r; c->eff_g = g; c->eff_b = b;
        return true;
    }
    return false;
}
