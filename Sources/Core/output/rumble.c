/*
 * Rumble motors in sim mode. Pure.
 *
 * WHY THESE CUES
 *   Pre-stall buffet is asymmetric on a real wing: one wing usually breaks
 *   before the other, and that is the cue that tells a pilot which way the
 *   aeroplane is about to drop. X-Plane exposes element_is_stalled per wing
 *   element, so sampling a few elements from each main wing gives genuine
 *   left/right asymmetry rather than one shared buzz.
 *
 *   The left motor is the heavy one and the right is light, so the same byte
 *   feels stronger on the left. That is left alone deliberately.
 *
 *   A crash is not a graded cue: it saturates both motors and holds, because
 *   anything subtler reads as "still flying".
 */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>

/* Thresholds come from feedback.conf (rumble_cfg_t). */

static unsigned char clamp8(double v) {
    if (!isfinite(v) || v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

static double unit(double v) {
    if (!isfinite(v) || v < 0) return 0;
    return v > 1 ? 1 : v;
}

void rumble_apply(const wx_t *wx, rumble_t *out, char *note, size_t note_n,
                  const rumble_cfg_t *cfg, double crash_age) {
    if (!cfg) cfg = &RUMBLE_DEFAULTS;
    if (wx->crashed) {
        /* full shake for crash_duration, then quiet: a crash must not buzz forever */
        bool shaking = cfg->crash_s > 0 && crash_age < cfg->crash_s;
        out->left = out->right = shaking ? 255 : 0;
        snprintf(note, note_n, "CRASH");
        return;
    }

    /* Per-wing stall dominates; the overall stall warning provides a symmetric
     * floor so a clean straight-ahead stall still buzzes both sides. */
    double sym = unit(wx->stall_ratio);
    double sl = unit(wx->stall_l), sr = unit(wx->stall_r);
    double l = fmax(sl, sym * cfg->stall_floor);
    double r = fmax(sr, sym * cfg->stall_floor);

    /* g-load jolt, symmetric - overstress is felt through the whole airframe */
    double gexc = (isfinite(wx->gn) ? fabs(wx->gn) : 0) - cfg->jolt_start_g;
    double jolt = 0;
    if (gexc > 0) {
        double span = cfg->jolt_full_g - cfg->jolt_start_g;
        jolt = span > 0 ? gexc / span : 1;
        if (jolt > 1) jolt = 1;
    }
    l = fmax(l, jolt);
    r = fmax(r, jolt);

    out->left  = clamp8(l * 255.0);
    out->right = clamp8(r * 255.0);

    if (jolt > 0.05)
        snprintf(note, note_n, "%.1f g load", wx->gn);
    else if (l > 0.02 || r > 0.02) {
        double diff = sl - sr;
        if (fabs(diff) < 0.12)
            snprintf(note, note_n, "stall buffet %.0f%%", fmax(l, r) * 100);
        else
            snprintf(note, note_n, "%s wing dropping %.0f%%",
                     diff > 0 ? "left" : "right", fmax(l, r) * 100);
    } else {
        snprintf(note, note_n, "clean");
    }
}
