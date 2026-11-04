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

/* above this load factor a hard landing or overstress adds a jolt */
#define G_JOLT      2.5
#define G_JOLT_FULL 6.0

static unsigned char clamp8(double v) {
    if (!isfinite(v) || v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

static double unit(double v) {
    if (!isfinite(v) || v < 0) return 0;
    return v > 1 ? 1 : v;
}

void rumble_apply(const wx_t *wx, rumble_t *out, char *note, size_t note_n) {
    if (wx->crashed) {
        out->left = out->right = 255;
        snprintf(note, note_n, "CRASH");
        return;
    }

    /* Per-wing stall dominates; the overall stall warning provides a symmetric
     * floor so a clean straight-ahead stall still buzzes both sides. */
    double sym = unit(wx->stall_ratio);
    double sl = unit(wx->stall_l), sr = unit(wx->stall_r);
    double l = fmax(sl, sym * 0.60);
    double r = fmax(sr, sym * 0.60);

    /* g-load jolt, symmetric - overstress is felt through the whole airframe */
    double gexc = (isfinite(wx->gn) ? fabs(wx->gn) : 0) - G_JOLT;
    double jolt = 0;
    if (gexc > 0) {
        jolt = gexc / (G_JOLT_FULL - G_JOLT);
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
