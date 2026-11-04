/*
 * Rumble motors, driven either by hand or by the flight model.
 *
 * WHY THESE CUES
 *   Pre-stall buffet is asymmetric on a real wing: one wing usually breaks
 *   before the other, and that is exactly the cue that tells a pilot which way
 *   the aeroplane is about to drop. X-Plane exposes
 *   sim/flightmodel2/wing/elements/element_is_stalled per element, so sampling
 *   a few elements from each main wing gives genuine left/right asymmetry
 *   rather than one shared buzz.
 *
 *   The left motor is the heavy one and the right is light, so the same byte
 *   feels stronger on the left. That is left alone deliberately - it matches
 *   how the hardware behaves and the asymmetry is the point.
 *
 *   A crash is not a graded cue. It saturates both motors and holds, because
 *   anything subtler reads as "still flying".
 */
#include "dsxp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* above this load factor a hard landing or overstress adds a jolt */
#define G_JOLT      2.5
#define G_JOLT_FULL 6.0

static unsigned char clamp8(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

void rumble_apply(void) {
    pthread_mutex_lock(&g_lock);

    if (g_wx.crashed) {
        g_rumble.left = g_rumble.right = 255;
        snprintf(g_rnote, sizeof g_rnote, "CRASH");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Per-wing stall dominates; the overall stall warning provides a symmetric
     * floor so a clean straight-ahead stall still buzzes both sides. */
    double sym = g_wx.stall_ratio;
    if (sym < 0) sym = 0;
    if (sym > 1) sym = 1;
    double l = fmax(g_wx.stall_l, sym * 0.60);
    double r = fmax(g_wx.stall_r, sym * 0.60);

    /* g-load jolt, symmetric - overstress is felt through the whole airframe */
    double gexc = fabs(g_wx.gn) - G_JOLT;
    double jolt = 0;
    if (gexc > 0) {
        jolt = gexc / (G_JOLT_FULL - G_JOLT);
        if (jolt > 1) jolt = 1;
    }
    l = fmax(l, jolt);
    r = fmax(r, jolt);

    g_rumble.left  = clamp8(l * 255.0);
    g_rumble.right = clamp8(r * 255.0);

    if (jolt > 0.05)
        snprintf(g_rnote, sizeof g_rnote, "%.1f g load", g_wx.gn);
    else if (l > 0.02 || r > 0.02) {
        double diff = g_wx.stall_l - g_wx.stall_r;
        if (fabs(diff) < 0.12)
            snprintf(g_rnote, sizeof g_rnote, "stall buffet %.0f%%", fmax(l, r) * 100);
        else
            snprintf(g_rnote, sizeof g_rnote, "%s wing dropping %.0f%%",
                     diff > 0 ? "left" : "right", fmax(l, r) * 100);
    } else {
        snprintf(g_rnote, sizeof g_rnote, "clean");
    }
    pthread_mutex_unlock(&g_lock);
}
