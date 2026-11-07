/*
 * Sim telemetry over X-Plane's RREF subscription, feeding wind-mode triggers,
 * sim-mode rumble and the sim-mode light bar. Pure: packets go out through
 * c->sinks.udp_send and come back in through core_on_udp().
 *
 * RREF: "RREF" 0x00, int32 frequency, int32 id, 400-byte dataref path. X-Plane
 * streams {int32 id, float value} pairs back to the sender's port at that rate;
 * frequency 0 cancels. One request per array element. (Laminar, "Exchanging
 * Data with X-Plane", in the X-Plane install's Instructions folder.)
 */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *SCALAR[RR_TURB0] = {
    [RR_IAS]    = "sim/flightmodel/position/indicated_airspeed",
    [RR_WSPD]   = "sim/weather/aircraft/wind_now_speed_msc",
    [RR_WDIR]   = "sim/weather/aircraft/wind_now_direction_degt",
    [RR_HDG]    = "sim/flightmodel/position/psi",
    [RR_STALLR] = "sim/cockpit2/annunciators/stall_warning_ratio",
    [RR_CRASH]  = "sim/flightmodel2/misc/has_crashed",
    [RR_GN]     = "sim/flightmodel/forces/g_nrml",
    [RR_PAUSED] = "sim/time/paused",
    [RR_ELEV]   = "sim/flightmodel/position/elevation",
};
static const int STALL_ELEMENT[STALL_SAMPLES] = { 2, 5, 8 };

int telemetry_rref_count(void) { return RR_COUNT - 1; }

bool telemetry_rref_path(int id, char *out, size_t n) {
    int w;
    if (id >= 1 && id < RR_TURB0)
        w = snprintf(out, n, "%s", SCALAR[id]);
    else if (id >= RR_TURB0 && id < RR_ALT0)
        w = snprintf(out, n, "sim/weather/aircraft/turbulence[%d]", id - RR_TURB0);
    else if (id >= RR_ALT0 && id < RR_STALL0)
        w = snprintf(out, n, "sim/weather/aircraft/wind_altitude_msl_m[%d]", id - RR_ALT0);
    else if (id >= RR_STALL0 && id < RR_COUNT) {
        int k = id - RR_STALL0, wing = k / STALL_SAMPLES, el = STALL_ELEMENT[k % STALL_SAMPLES];
        w = snprintf(out, n, "sim/flightmodel2/wing/elements/element_is_stalled[%d]", wing * 10 + el);
    } else return false;
    return w > 0 && (size_t)w < n;
}

/* Turbulence at the aircraft: X-Plane 12 publishes it per weather layer, with
 * each layer's center altitude. Interpolate linearly between the two layers
 * around the aircraft, holding the end values beyond them. If the altitudes are
 * all zero (not filled in), fall back to the first layer. */
double turb_at_altitude(const float *turb, const float *alt, int n, double elev_m) {
    int idx[WX_LAYERS], m = 0;
    bool any_alt = false;
    for (int i = 0; i < n && i < WX_LAYERS; i++) {
        if (!isfinite(turb[i]) || !isfinite(alt[i])) continue;
        idx[m++] = i;
        if (alt[i] != 0) any_alt = true;
    }
    double r;
    if (m == 0) r = 0;
    else if (!any_alt || !isfinite(elev_m)) r = turb[idx[0]];
    else {
        for (int i = 1; i < m; i++) {                 /* sort by altitude */
            int v = idx[i], j = i - 1;
            while (j >= 0 && alt[idx[j]] > alt[v]) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = v;
        }
        if (elev_m <= alt[idx[0]]) r = turb[idx[0]];
        else if (elev_m >= alt[idx[m - 1]]) r = turb[idx[m - 1]];
        else {
            r = turb[idx[m - 1]];
            for (int j = 0; j + 1 < m; j++) {
                double a0 = alt[idx[j]], a1 = alt[idx[j + 1]];
                if (elev_m >= a0 && elev_m <= a1) {
                    r = a1 > a0 ? turb[idx[j]] + (turb[idx[j + 1]] - turb[idx[j]]) * (elev_m - a0) / (a1 - a0)
                                : turb[idx[j]];
                    break;
                }
            }
        }
    }
    if (!isfinite(r) || r < 0) r = 0;
    if (r > 10) r = 10;
    return r;
}

static void send_rref_all(core_t *c, int32_t freq) {
    if (!c->sinks.udp_send) return;
    for (int i = 1; i < RR_COUNT; i++) {
        char path[128];
        if (!telemetry_rref_path(i, path, sizeof path)) continue;
        unsigned char p[RREF_LEN];
        size_t n = xp_build_rref(p, freq, i, path);
        c->sinks.udp_send(c->sinks.ctx, p, n);
    }
}

void telemetry_unsubscribe(core_t *c) {
    if (!c->subscribed) return;
    send_rref_all(c, 0);                  /* frequency 0 cancels */
    c->subscribed = false;
}

/*
 * Simulator-set effects latch on the controller: the last report sent stays in
 * force until another one replaces it. So when telemetry stops - X-Plane quits,
 * or the link drops - the computed values must be cleared, or a crash rumble
 * carries on after the sim is gone.
 */
void sim_release(core_t *c, const char *why) {
    c->wx.live = false;
    c->wx.crashed = false;               /* a stale crash flag would re-arm */
    c->wx.paused = false;
    c->wx.stall_l = c->wx.stall_r = c->wx.stall_ratio = 0;
    c->wx.ias = c->wx.turb = c->wx.gn = 0;
    memset(c->stall_el, 0, sizeof c->stall_el);
    if (c->s.rumble_source == RUMBLE_SRC_SIM) c->rumble.left = c->rumble.right = 0;
    if (c->s.trigger_source == TRIGGER_SRC_WIND) {
        c->fx_out[0] = TFX_NONE;
        c->fx_out[1] = TFX_NONE;
    }
    snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "%s", why);
    if (c->s.rumble_source == RUMBLE_SRC_SIM) snprintf(c->rumble_note, sizeof c->rumble_note, "%s", why);
    out_mark(c);
}

static double wing_mean(const float *s) {
    double t = 0;
    for (int k = 0; k < STALL_SAMPLES; k++) t += s[k];
    return t / STALL_SAMPLES;
}

void core_on_udp(core_t *c, const unsigned char *buf, size_t n, bool from_loopback, double now) {
    if (!from_loopback) return;           /* only X-Plane on this Mac may feed us */
    rref_pair_t pairs[128];
    int k = telemetry_parse(buf, n, pairs, 128);
    if (k <= 0) return;
    bool stall = false, weather = false;
    for (int i = 0; i < k; i++) {
        float v = pairs[i].value;
        int id = pairs[i].id;
        if (!isfinite(v)) continue;
        switch (id) {
        case RR_IAS:    c->wx.ias = v;           break;
        case RR_WSPD:   c->wx.wind_ms = v;       break;
        case RR_WDIR:   c->wx.wind_dir = v;      break;
        case RR_HDG:    c->wx.heading = v;       break;
        case RR_STALLR: c->wx.stall_ratio = v;   break;
        case RR_CRASH:  c->wx.crashed = v > 0.5f; break;
        case RR_GN:     c->wx.gn = v;            break;
        case RR_PAUSED: c->wx.paused = v > 0.5f; break;
        case RR_ELEV:   c->wx.elev_m = v; weather = true; break;
        default:
            if (id >= RR_TURB0 && id < RR_ALT0) { c->turb_layer[id - RR_TURB0] = v; weather = true; }
            else if (id >= RR_ALT0 && id < RR_STALL0) { c->alt_layer[id - RR_ALT0] = v; weather = true; }
            else if (id >= RR_STALL0 && id < RR_COUNT) {
                int s = id - RR_STALL0;
                c->stall_el[s / STALL_SAMPLES][s % STALL_SAMPLES] = v;
                stall = true;
            }
            break;
        }
    }
    if (weather) c->wx.turb = turb_at_altitude(c->turb_layer, c->alt_layer, WX_LAYERS, c->wx.elev_m);
    if (stall) {
        /* the main wing may be modeled as Plane Maker wing 1 or 2: take the worse */
        double w[STALL_WINGS];
        for (int i = 0; i < STALL_WINGS; i++) w[i] = wing_mean(c->stall_el[i]);
        c->wx.stall_l = fmax(w[0], w[2]);
        c->wx.stall_r = fmax(w[1], w[3]);
    }
    c->wx.last_rx = now;
    if (!c->wx.live) c->force_gate = true;   /* X-Plane just started answering */
    c->wx.live = true;
}

/* Subscribed whenever X-Plane is running: one feed serves wind-mode triggers,
 * sim-mode rumble and the sim-mode light bar, and its arrival is also the proof
 * that X-Plane is answering on its network port. */
void telemetry_tick(core_t *c, double now) {
    bool want = c->xp_running && !c->suspended && !c->dry_run;

    if (want && !c->subscribed) {
        send_rref_all(c, (int32_t)lround(c->bridge.telemetry_hz));
        c->subscribed = true;
        c->last_sub = now;
    }
    if (!want && c->subscribed) {
        telemetry_unsubscribe(c);
        sim_release(c, c->xp_running ? "Paused" : "X-Plane closed");
    }
    if (!want) {
        if (c->wx.live) sim_release(c, "X-Plane closed");
        snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "%s",
                 c->s.trigger_source == TRIGGER_SRC_WIND ? "Waiting for X-Plane" : "Manual");
        if (c->s.rumble_source == RUMBLE_SRC_SIM)
            snprintf(c->rumble_note, sizeof c->rumble_note, "Waiting for X-Plane");
        return;
    }
    /* X-Plane does not always keep subscriptions (observed across reloads; not
     * documented). Resend every few seconds until data actually arrives. */
    if (!c->wx.live && now - c->last_sub > c->bridge.telemetry_resub_s) {
        send_rref_all(c, (int32_t)lround(c->bridge.telemetry_hz));
        c->last_sub = now;
    }

    bool was_live = c->wx.live;
    if (c->wx.live && now - c->wx.last_rx > c->bridge.telemetry_timeout_s) c->wx.live = false;
    /* crash shake is timed from the moment has_crashed became true */
    if (c->wx.crashed && !c->crash_seen) { c->crash_seen = true; c->crash_at = now; }
    if (!c->wx.crashed) c->crash_seen = false;

    if (c->wx.live && c->wx.paused) {
        /* paused: the sim keeps answering, but nothing should buzz or resist */
        if (c->s.rumble_source == RUMBLE_SRC_SIM && (c->rumble.left || c->rumble.right)) {
            c->rumble.left = c->rumble.right = 0;
            out_mark(c);
        }
        if (c->s.trigger_source == TRIGGER_SRC_WIND && (c->fx_out[0].mode != TFX_OFF || c->fx_out[1].mode != TFX_OFF)) {
            c->fx_out[0] = c->fx_out[1] = TFX_NONE;
            out_mark(c);
        }
        snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "Paused");
        if (c->s.rumble_source == RUMBLE_SRC_SIM) snprintf(c->rumble_note, sizeof c->rumble_note, "Paused");
    } else if (c->wx.live) {
        if (c->s.trigger_source == TRIGGER_SRC_WIND) {
            trigger_fx_t before[2] = { c->fx_out[0], c->fx_out[1] };
            wind_apply(&c->wx, c->fx_out, &c->s.wind);
            if (!fx_equal(&before[0], &c->fx_out[0]) || !fx_equal(&before[1], &c->fx_out[1])) out_mark(c);
        } else {
            trigger_fx_t scratch[2];
            wind_apply(&c->wx, scratch, &c->s.wind);     /* keeps load/crosswind readouts live */
        }
        if (c->s.rumble_source == RUMBLE_SRC_SIM) {
            rumble_t before = c->rumble;
            rumble_apply(&c->wx, &c->rumble, c->rumble_note, sizeof c->rumble_note, &c->s.rumble, now - c->crash_at);
            if (before.left != c->rumble.left || before.right != c->rumble.right) out_mark(c);
        }
    } else if (was_live) {
        /* the feed went quiet while the process is still around - hung, or
         * networking turned off. Do not leave anything latched. */
        sim_release(c, "Telemetry lost");
    } else {
        snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "Waiting for X-Plane");
        if (c->s.rumble_source == RUMBLE_SRC_SIM)
            snprintf(c->rumble_note, sizeof c->rumble_note, "Waiting for X-Plane");
    }
}
