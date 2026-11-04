/*
 * Wind source: set the adaptive triggers from X-Plane's own flight state. Pure.
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

/* All numbers come from feedback.conf (wind_cfg_t); the comments there say what
 * each one does when flying. */
#define KT_TO_MS 0.514444

static unsigned char clamp8(double v) {
    if (!isfinite(v) || v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

void wind_apply(wx_t *wx, trigger_fx_t fx[2], const wind_cfg_t *cfg) {
    if (!cfg) cfg = &WIND_DEFAULTS;
    double ias = (isfinite(wx->ias) && wx->ias > 0) ? wx->ias : 0;
    double q = ias / (cfg->full_force_kt > 0 ? cfg->full_force_kt : 160.0);
    if (q > 1.0) q = 1.0;
    q = q * q;                                   /* hinge moment goes as IAS^2 */
    wx->load = q;

    /* crosswind component, positive = blowing from the right */
    double rel = (wx->wind_dir - wx->heading) * M_PI / 180.0;
    double xw = wx->wind_ms * sin(rel);
    if (!isfinite(xw)) xw = 0;
    wx->crosswind = xw;
    double asym = xw / (cfg->crosswind_full_kt * KT_TO_MS);
    if (asym >  1) asym =  1;
    if (asym < -1) asym = -1;

    double base = (cfg->base_force + cfg->airspeed_force * q) * 255.0;   /* byte-space force */
    /* wind from the right loads the left rudder, and unloads the right */
    double fl = base * (1.0 + cfg->crosswind_bias * asym);
    double fr = base * (1.0 - cfg->crosswind_bias * asym);

    if (wx->turb > cfg->buffet_turbulence && cfg->buffet_turbulence < 10.0) {
        double t = (wx->turb - cfg->buffet_turbulence) / (10.0 - cfg->buffet_turbulence);
        if (t > 1) t = 1;
        unsigned char amp = clamp8((cfg->buffet_strength_min + (cfg->buffet_strength_max - cfg->buffet_strength_min) * t) * 255.0);
        unsigned char hz  = clamp8(cfg->buffet_freq_min_hz + (cfg->buffet_freq_max_hz - cfg->buffet_freq_min_hz) * t);
        unsigned char at  = clamp8(cfg->buffet_start * 255.0);
        if (hz < 1) hz = 1;
        fx[0] = (trigger_fx_t){ TFX_VIBRATE, at, hz, amp };
        fx[1] = (trigger_fx_t){ TFX_VIBRATE, at, hz, amp };
        snprintf(wx->trigger_note, sizeof wx->trigger_note, "Buffet, turbulence %.1f", wx->turb);
    } else if (ias < cfg->no_airflow_kt) {
        fx[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        fx[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        snprintf(wx->trigger_note, sizeof wx->trigger_note, "No airflow");
    } else {
        unsigned char free_play = clamp8(cfg->free_play * 255.0);
        fx[0] = (trigger_fx_t){ TFX_RESIST, free_play, 0, clamp8(fl) };
        fx[1] = (trigger_fx_t){ TFX_RESIST, free_play, 0, clamp8(fr) };
        snprintf(wx->trigger_note, sizeof wx->trigger_note, "Airflow load %.0f%%", q * 100);
    }
}
