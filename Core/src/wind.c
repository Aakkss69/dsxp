/*
 * Wind mode: drive the adaptive triggers from X-Plane's own flight state.
 *
 * WHY THESE EFFECTS
 *   In this profile L2/R2 are joy_use_rudd_lft / joy_use_rudd_rgt - differential
 *   rudder. On a real aeroplane the force on a rudder pedal is the aerodynamic
 *   hinge moment of the rudder, which scales with dynamic pressure
 *   q = 1/2 rho V^2. Indicated airspeed is calibrated so that 1/2 rho0 IAS^2 = q,
 *   so resistance proportional to IAS^2 is the physically correct law, not a
 *   guess. That is the same cue force-feedback pedals reproduce: pedal force
 *   builds as speed builds on the takeoff roll.
 *
 *   Crosswind adds asymmetry. Holding rudder into the wind loads one pedal more
 *   than the other, so the crosswind component biases the two triggers apart.
 *
 *   Turbulence is buffet, and buffet is a vibration, not a force. Above a
 *   threshold it takes over the trigger entirely, because in a real aircraft
 *   airframe buffet dominates what your feet feel.
 *
 * MODE CHOICE
 *   Sony's firmware exposes official effects Feedback 0x21, Weapon 0x25 and
 *   Vibration 0x26 alongside the simpler legacy 0x01 / 0x02 / 0x06. The legacy
 *   ones are what this build already drives and what has been confirmed working
 *   on this hardware, so wind mode uses Resist (0x01) and Vibrate (0x06).
 *
 * TRANSPORT
 *   X-Plane's RREF subscription: send "RREF" + 0x00, int32 frequency, int32 id,
 *   then a 400-byte dataref path. X-Plane streams {int32 id, float value} pairs
 *   back to the source port at that rate.
 */
#include "dsxp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

wx_t g_wx;

static void wind_subscribe(int sock, const struct sockaddr_in *xp);
static void wind_unsubscribe(int sock, const struct sockaddr_in *xp);
static void wind_poll(int sock);
static void wind_apply(void);

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
     * root and tip, which stall last and first respectively. The UI shows both
     * sides so this indexing is verifiable by stalling one wing. */
    "sim/flightmodel2/wing/elements/element_is_stalled[2]",
    "sim/flightmodel2/wing/elements/element_is_stalled[5]",
    "sim/flightmodel2/wing/elements/element_is_stalled[8]",
    "sim/flightmodel2/wing/elements/element_is_stalled[12]",
    "sim/flightmodel2/wing/elements/element_is_stalled[15]",
    "sim/flightmodel2/wing/elements/element_is_stalled[18]",
};

/* Reference speed the load curve saturates against. 160 kias is a reasonable
 * light-aircraft full-scale; faster aircraft just reach full force sooner. */
#define VREF_KIAS      160.0
#define XWIND_FULL_MS   12.0    /* crosswind giving full asymmetry            */
#define TURB_BUFFET      1.5    /* above this, buffet takes the trigger over  */
#define RESIST_START      30    /* a little free play before force builds     */

static void wind_subscribe(int sock, const struct sockaddr_in *xp) {
    for (int i = 1; i < RR_COUNT; i++) {
        unsigned char pkt[413];
        memset(pkt, 0, sizeof pkt);
        memcpy(pkt, "RREF", 4);
        int32_t freq = 10, id = i;
        memcpy(pkt + 5, &freq, 4);
        memcpy(pkt + 9, &id, 4);
        strncpy((char *)(pkt + 13), DREF[i], 399);
        sendto(sock, pkt, sizeof pkt, 0, (const struct sockaddr *)xp, sizeof *xp);
    }
    if (g_verbose)
        fprintf(stderr, "[dsxp] wind: subscribed to %d datarefs\n", RR_COUNT - 1);
}

static void wind_unsubscribe(int sock, const struct sockaddr_in *xp) {
    for (int i = 1; i < RR_COUNT; i++) {
        unsigned char pkt[413];
        memset(pkt, 0, sizeof pkt);
        memcpy(pkt, "RREF", 4);
        int32_t freq = 0, id = i;       /* frequency 0 cancels the subscription */
        memcpy(pkt + 5, &freq, 4);
        memcpy(pkt + 9, &id, 4);
        strncpy((char *)(pkt + 13), DREF[i], 399);
        sendto(sock, pkt, sizeof pkt, 0, (const struct sockaddr *)xp, sizeof *xp);
    }
}

/* Consume whatever RREF responses are queued. Non-blocking; safe to call every
 * frame whether or not X-Plane is talking to us. */
static void wind_poll(int sock) {
    unsigned char buf[2048];
    static float sl[3], sr[3];
    bool got_l = false, got_r = false;
    for (int guard = 0; guard < 32; guard++) {
        ssize_t n = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 13 || memcmp(buf, "RREF", 4) != 0) {
            if (n <= 0) break;
            continue;
        }
        pthread_mutex_lock(&g_lock);
        for (ssize_t o = 5; o + 8 <= n; o += 8) {
            int32_t id;
            float v;
            memcpy(&id, buf + o, 4);
            memcpy(&v, buf + o + 4, 4);
            switch (id) {
            case RR_IAS:  g_wx.ias = v;        break;
            case RR_WSPD: g_wx.wind_ms = v;    break;
            case RR_WDIR: g_wx.wind_dir = v;   break;
            case RR_HDG:  g_wx.heading = v;    break;
            case RR_TURB: g_wx.turb = v;       break;
            case RR_STALLR: g_wx.stall_ratio = v; break;
            case RR_CRASH:  g_wx.crashed = v > 0.5; break;
            case RR_GN:     g_wx.gn = v;       break;
            case RR_SL0: case RR_SL1: case RR_SL2:
                sl[id - RR_SL0] = v; got_l = true; break;
            case RR_SR0: case RR_SR1: case RR_SR2:
                sr[id - RR_SR0] = v; got_r = true; break;
            default: break;
            }
        }
        if (got_l) g_wx.stall_l = (sl[0] + sl[1] + sl[2]) / 3.0;
        if (got_r) g_wx.stall_r = (sr[0] + sr[1] + sr[2]) / 3.0;
        g_wx.last_rx = now_sec();
        g_wx.live = true;
        pthread_mutex_unlock(&g_lock);
    }
    if (g_wx.live && now_sec() - g_wx.last_rx > 3.0) g_wx.live = false;
}

static unsigned char clamp8(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

/*
 * Sim-driven effects latch on the controller: the last report we sent stays in
 * force until we send another. So when telemetry stops - X-Plane quits, pauses,
 * or the link drops - the last computed values must be cleared, or a crash
 * rumble carries on after the sim is gone with nothing left to turn it off.
 */
static void sim_release(const char *why) {
    pthread_mutex_lock(&g_lock);
    g_wx.live = false;
    /* a stale crash flag would re-arm the moment anything read it again */
    g_wx.crashed = false;
    g_wx.stall_l = g_wx.stall_r = g_wx.stall_ratio = 0;
    g_wx.ias = g_wx.turb = g_wx.gn = 0;
    if (g_rmode == RM_SIM) g_rumble.left = g_rumble.right = 0;
    if (g_fxmode == FXM_WIND) {
        g_fx[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        g_fx[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
    }
    snprintf(g_wx.note, sizeof g_wx.note, "%s", why);
    if (g_rmode == RM_SIM) snprintf(g_rnote, sizeof g_rnote, "%s", why);
    pthread_mutex_unlock(&g_lock);
    out_mark();
}

/* Subscribe only while wind mode is selected and the sim is up; cancel when
 * either goes away so we are not shouting at a dead port. */
void wind_tick(void) {
    static bool subscribed = false;
    static double last_sub = 0;
    /* one subscription serves both sim-driven panels */
    bool want = (g_fxmode == FXM_WIND || g_rmode == RM_SIM) && g_xp_running;
    int sock = xp_socket();
    const struct sockaddr_in *xp = (const struct sockaddr_in *)xp_addr();

    if (want && !subscribed) {
        wind_subscribe(sock, xp);
        subscribed = true;
        last_sub = now_sec();
    }
    if (!want && subscribed) {
        wind_unsubscribe(sock, xp);
        subscribed = false;
        sim_release(g_xp_running ? "manual" : "X-Plane closed");
    }
    if (!want) {
        snprintf(g_wx.note, sizeof g_wx.note, "%s",
                 g_fxmode == FXM_WIND ? "waiting for X-Plane" : "manual");
        if (g_rmode == RM_SIM)
            snprintf(g_rnote, sizeof g_rnote, "waiting for X-Plane");
        return;
    }
    /* A subscription is not guaranteed to stick: X-Plane drops them across a
     * reload, and it will silently ignore a peer it has misfiled. Resend every
     * few seconds until data actually arrives, so the link heals itself instead
     * of sitting on "waiting for X-Plane" forever. */
    if (!g_wx.live && now_sec() - last_sub > 5.0) {
        wind_subscribe(sock, xp);
        last_sub = now_sec();
    }

    bool was_live = g_wx.live;
    wind_poll(sock);
    if (g_wx.live) {
        if (g_fxmode == FXM_WIND) { wind_apply();   out_mark(); }
        if (g_rmode == RM_SIM)    { rumble_apply(); out_mark(); }
    } else if (was_live) {
        /* the feed just went quiet while the process is still around - paused,
         * hung, or networking turned off. Same rule: do not leave it latched. */
        sim_release("telemetry lost");
    } else {
        snprintf(g_wx.note, sizeof g_wx.note, "waiting for X-Plane");
        if (g_rmode == RM_SIM)
            snprintf(g_rnote, sizeof g_rnote, "waiting for X-Plane");
    }
}

/* Translate flight state into the two trigger effects. */
static void wind_apply(void) {
    pthread_mutex_lock(&g_lock);

    double ias = g_wx.ias < 0 ? 0 : g_wx.ias;
    double q = ias / VREF_KIAS;
    if (q > 1.0) q = 1.0;
    q = q * q;                                   /* hinge moment goes as IAS^2 */
    g_wx.load = q;

    /* crosswind component, positive = blowing from the right */
    double rel = (g_wx.wind_dir - g_wx.heading) * M_PI / 180.0;
    double xw = g_wx.wind_ms * sin(rel);
    g_wx.crosswind = xw;
    double asym = xw / XWIND_FULL_MS;
    if (asym >  1) asym =  1;
    if (asym < -1) asym = -1;

    double base = 30.0 + 200.0 * q;              /* byte-space force */
    /* wind from the right loads the left rudder, and unloads the right */
    double fl = base * (1.0 + 0.45 * asym);
    double fr = base * (1.0 - 0.45 * asym);

    if (g_wx.turb > TURB_BUFFET) {
        double t = (g_wx.turb - TURB_BUFFET) / (10.0 - TURB_BUFFET);
        if (t > 1) t = 1;
        unsigned char amp = clamp8(60 + 180 * t);
        unsigned char hz  = clamp8(18 + 22 * t);
        g_fx[0] = (trigger_fx_t){ TFX_VIBRATE, 20, hz, amp };
        g_fx[1] = (trigger_fx_t){ TFX_VIBRATE, 20, hz, amp };
        snprintf(g_wx.note, sizeof g_wx.note, "buffet %.1f/10", g_wx.turb);
    } else if (ias < 5.0) {
        g_fx[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        g_fx[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        snprintf(g_wx.note, sizeof g_wx.note, "static - no airflow");
    } else {
        g_fx[0] = (trigger_fx_t){ TFX_RESIST, RESIST_START, 0, clamp8(fl) };
        g_fx[1] = (trigger_fx_t){ TFX_RESIST, RESIST_START, 0, clamp8(fr) };
        snprintf(g_wx.note, sizeof g_wx.note, "aero load %.0f%%", q * 100);
    }
    pthread_mutex_unlock(&g_lock);
}
