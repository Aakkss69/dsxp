/*
 * Wind mode: drive the adaptive triggers from X-Plane's own flight state. Pure.
 *
 * WHY THESE EFFECTS
 *   In the default profile L2/R2 are differential rudder. On a real aeroplane the
 *   force on a rudder pedal is the rudder's aerodynamic hinge moment, which
 *   scales with dynamic pressure q = 1/2 rho V^2. Indicated airspeed is
 *   calibrated so that 1/2 rho0 IAS^2 = q, so resistance proportional to IAS^2 is
 *   the physically correct law - the same cue force-feedback pedals reproduce.
 *
 *   Crosswind adds asymmetry: holding rudder into the wind loads one pedal more
 *   than the other, so the crosswind component biases the two triggers apart.
 *
 *   Turbulence is buffet, and buffet is a vibration, not a force. Above a
 *   threshold it takes the trigger over entirely.
 *
 * Effects used: Feedback (TFX_RESIST, 0x21) and Vibration (TFX_VIBRATE, 0x26),
 * built by put_effect() in output.c.
 */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>

/* Reference speed the load curve saturates against. 160 kias is a reasonable
 * light-aircraft full-scale; faster aircraft just reach full force sooner. */
#define VREF_KIAS      160.0
#define XWIND_FULL_MS   12.0    /* crosswind giving full asymmetry            */
#define TURB_BUFFET      1.5    /* above this, buffet takes the trigger over  */
#define RESIST_START      30    /* a little free play before force builds     */

static unsigned char clamp8(double v) {
    if (!isfinite(v) || v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

void wind_apply(wx_t *wx, trigger_fx_t fx[2]) {
    double ias = (isfinite(wx->ias) && wx->ias > 0) ? wx->ias : 0;
    double q = ias / VREF_KIAS;
    if (q > 1.0) q = 1.0;
    q = q * q;                                   /* hinge moment goes as IAS^2 */
    wx->load = q;

    /* crosswind component, positive = blowing from the right */
    double rel = (wx->wind_dir - wx->heading) * M_PI / 180.0;
    double xw = wx->wind_ms * sin(rel);
    if (!isfinite(xw)) xw = 0;
    wx->crosswind = xw;
    double asym = xw / XWIND_FULL_MS;
    if (asym >  1) asym =  1;
    if (asym < -1) asym = -1;

    double base = 30.0 + 200.0 * q;              /* byte-space force */
    /* wind from the right loads the left rudder, and unloads the right */
    double fl = base * (1.0 + 0.45 * asym);
    double fr = base * (1.0 - 0.45 * asym);

    if (wx->turb > TURB_BUFFET) {
        double t = (wx->turb - TURB_BUFFET) / (10.0 - TURB_BUFFET);
        if (t > 1) t = 1;
        unsigned char amp = clamp8(60 + 180 * t);
        unsigned char hz  = clamp8(18 + 22 * t);
        fx[0] = (trigger_fx_t){ TFX_VIBRATE, 20, hz, amp };
        fx[1] = (trigger_fx_t){ TFX_VIBRATE, 20, hz, amp };
        snprintf(wx->note, sizeof wx->note, "buffet %.1f/10", wx->turb);
    } else if (ias < 5.0) {
        fx[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        fx[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        snprintf(wx->note, sizeof wx->note, "static - no airflow");
    } else {
        fx[0] = (trigger_fx_t){ TFX_RESIST, RESIST_START, 0, clamp8(fl) };
        fx[1] = (trigger_fx_t){ TFX_RESIST, RESIST_START, 0, clamp8(fr) };
        snprintf(wx->note, sizeof wx->note, "aero load %.0f%%", q * 100);
    }
}
