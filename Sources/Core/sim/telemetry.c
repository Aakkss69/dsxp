/*
 * Sim telemetry over X-Plane's RREF subscription, feeding wind-mode triggers,
 * sim-mode rumble and the sim-mode light bar. Pure: packets go out through
 * c->sinks.udp_send and come back in through core_on_udp().
 */
#include "core_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* subscription ids */
enum { RR_IAS = 1, RR_WSPD, RR_WDIR, RR_HDG, RR_TURB,
       RR_STALLR, RR_CRASH, RR_GN,
       RR_SL0, RR_SL1, RR_SL2,          /* left wing element samples  */
       RR_SR0, RR_SR1, RR_SR2,          /* right wing element samples */
       RR_COUNT };

static const char *DREF[RR_COUNT] = {
    NULL,
    "sim/flightmodel/position/indicated_airspeed",
    "sim/weather/aircraft/wind_now_speed_msc",
    "sim/weather/aircraft/wind_now_direction_degt",
    "sim/flightmodel/position/psi",
    "sim/weather/aircraft/turbulence[0]",
    "sim/cockpit2/annunciators/stall_warning_ratio",
    "sim/flightmodel2/misc/has_crashed",
    "sim/flightmodel/forces/g_nrml",
    /* element_is_stalled is float[480]; X-Plane lays wings out in blocks of ten,
     * so 0-9 is the left main and 10-19 the right. Mid-span samples avoid the
     * root and tip, which stall last and first respectively. */
    "sim/flightmodel2/wing/elements/element_is_stalled[2]",
    "sim/flightmodel2/wing/elements/element_is_stalled[5]",
    "sim/flightmodel2/wing/elements/element_is_stalled[8]",
    "sim/flightmodel2/wing/elements/element_is_stalled[12]",
    "sim/flightmodel2/wing/elements/element_is_stalled[15]",
    "sim/flightmodel2/wing/elements/element_is_stalled[18]",
};

static void send_rref_all(core_t *c, int32_t freq) {
    if (!c->sinks.udp_send) return;
    for (int i = 1; i < RR_COUNT; i++) {
        unsigned char p[RREF_LEN];
        size_t n = xp_build_rref(p, freq, i, DREF[i]);
        c->sinks.udp_send(c->sinks.ctx, p, n);
    }
}

void telemetry_unsubscribe(core_t *c) {
    if (!c->subscribed) return;
    send_rref_all(c, 0);                  /* frequency 0 cancels */
    c->subscribed = false;
}

/*
 * Sim-driven effects latch on the controller: the last report sent stays in
 * force until another one replaces it. So when telemetry stops - X-Plane quits,
 * pauses, or the link drops - the computed values must be cleared, or a crash
 * rumble carries on after the sim is gone.
 */
void sim_release(core_t *c, const char *why) {
    c->wx.live = false;
    c->wx.crashed = false;               /* a stale crash flag would re-arm */
    c->wx.stall_l = c->wx.stall_r = c->wx.stall_ratio = 0;
    c->wx.ias = c->wx.turb = c->wx.gn = 0;
    memset(c->sl, 0, sizeof c->sl);
    memset(c->sr, 0, sizeof c->sr);
    if (c->s.rmode == RM_SIM) c->rumble.left = c->rumble.right = 0;
    if (c->s.fxmode == FXM_WIND) {
        c->fx_out[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        c->fx_out[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
    }
    snprintf(c->wx.note, sizeof c->wx.note, "%s", why);
    if (c->s.rmode == RM_SIM) snprintf(c->rnote, sizeof c->rnote, "%s", why);
    out_mark(c);
}

void core_on_udp(core_t *c, const unsigned char *buf, size_t n, bool from_loopback, double now) {
    if (!from_loopback) return;           /* only X-Plane on this Mac may feed us */
    rref_pair_t pairs[64];
    int k = telemetry_parse(buf, n, pairs, 64);
    if (k <= 0) return;
    bool got_l = false, got_r = false;
    for (int i = 0; i < k; i++) {
        float v = pairs[i].value;
        if (!isfinite(v)) continue;
        switch (pairs[i].id) {
        case RR_IAS:    c->wx.ias = v;          break;
        case RR_WSPD:   c->wx.wind_ms = v;      break;
        case RR_WDIR:   c->wx.wind_dir = v;     break;
        case RR_HDG:    c->wx.heading = v;      break;
        case RR_TURB:   c->wx.turb = v;         break;
        case RR_STALLR: c->wx.stall_ratio = v;  break;
        case RR_CRASH:  c->wx.crashed = v > 0.5f; break;
        case RR_GN:     c->wx.gn = v;           break;
        case RR_SL0: case RR_SL1: case RR_SL2:
            c->sl[pairs[i].id - RR_SL0] = v; got_l = true; break;
        case RR_SR0: case RR_SR1: case RR_SR2:
            c->sr[pairs[i].id - RR_SR0] = v; got_r = true; break;
        default: break;
        }
    }
    if (got_l) c->wx.stall_l = (c->sl[0] + c->sl[1] + c->sl[2]) / 3.0;
    if (got_r) c->wx.stall_r = (c->sr[0] + c->sr[1] + c->sr[2]) / 3.0;
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
        send_rref_all(c, TELEMETRY_HZ);
        c->subscribed = true;
        c->last_sub = now;
    }
    if (!want && c->subscribed) {
        telemetry_unsubscribe(c);
        sim_release(c, c->xp_running ? "paused" : "X-Plane closed");
    }
    if (!want) {
        if (c->wx.live) sim_release(c, "X-Plane closed");
        snprintf(c->wx.note, sizeof c->wx.note, "%s",
                 c->s.fxmode == FXM_WIND ? "waiting for X-Plane" : "manual");
        if (c->s.rmode == RM_SIM)
            snprintf(c->rnote, sizeof c->rnote, "waiting for X-Plane");
        return;
    }
    /* A subscription is not guaranteed to stick: X-Plane drops them across a
     * reload. Resend every few seconds until data actually arrives. */
    if (!c->wx.live && now - c->last_sub > TELEMETRY_RESUB_SEC) {
        send_rref_all(c, TELEMETRY_HZ);
        c->last_sub = now;
    }

    bool was_live = c->wx.live;
    if (c->wx.live && now - c->wx.last_rx > TELEMETRY_TIMEOUT_SEC) c->wx.live = false;

    if (c->wx.live) {
        if (c->s.fxmode == FXM_WIND) {
            trigger_fx_t before[2] = { c->fx_out[0], c->fx_out[1] };
            wind_apply(&c->wx, c->fx_out);
            if (!fx_equal(&before[0], &c->fx_out[0]) || !fx_equal(&before[1], &c->fx_out[1])) out_mark(c);
        } else {
            trigger_fx_t scratch[2];
            wind_apply(&c->wx, scratch);     /* keeps load/crosswind readouts live */
        }
        if (c->s.rmode == RM_SIM) {
            rumble_t before = c->rumble;
            rumble_apply(&c->wx, &c->rumble, c->rnote, sizeof c->rnote);
            if (before.left != c->rumble.left || before.right != c->rumble.right) out_mark(c);
        }
    } else if (was_live) {
        /* the feed went quiet while the process is still around - paused, hung,
         * or networking turned off. Do not leave anything latched. */
        sim_release(c, "telemetry lost");
    } else {
        snprintf(c->wx.note, sizeof c->wx.note, "waiting for X-Plane");
        if (c->s.rmode == RM_SIM)
            snprintf(c->rnote, sizeof c->rnote, "waiting for X-Plane");
    }
}
