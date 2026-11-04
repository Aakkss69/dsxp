/*
 * Writing TO the DualSense: light bar, player LEDs, adaptive triggers.
 *
 * Verified working on this machine - a plain userspace process can drive the
 * device over Bluetooth even while Apple's AppleGCHIDUserEventDriver holds it.
 *
 * Bluetooth output report 0x31, 78 bytes:
 *   [0]      report id 0x31
 *   [1]      seq_tag (high nibble)
 *   [2]      tag 0x10
 *   [3..]    common block
 *   [74..77] CRC-32 over 0xA2 + report[0..73], poly 0xEDB88320, little endian
 *
 * Get the CRC wrong and the controller silently discards the packet - it does
 * not error, it just ignores you.
 */
#include "dsxp.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define COMMON       3
#define OFF_VALID0   (COMMON + 0)
#define OFF_VALID1   (COMMON + 1)
#define OFF_RUMBLE_R (COMMON + 2)
#define OFF_RUMBLE_L (COMMON + 3)
/* reserved2 in the Linux struct is really the two trigger effect blocks */
#define OFF_TRIG_R   (COMMON + 10)   /* 11 bytes: mode + 10 params */
#define OFF_TRIG_L   (COMMON + 21)
#define OFF_VALID2   (COMMON + 38)
#define OFF_LB_SETUP (COMMON + 41)
#define OFF_BRIGHT   (COMMON + 42)
#define OFF_PLAYER   (COMMON + 43)
#define OFF_RED      (COMMON + 44)
#define OFF_GREEN    (COMMON + 45)
#define OFF_BLUE     (COMMON + 46)

/* valid_flag0 */
#define F0_COMPAT_VIBRATION 0x01
#define F0_HAPTICS_SELECT   0x02
#define F0_TRIGGER_R        0x04
#define F0_TRIGGER_L        0x08
/* valid_flag1 */
#define F1_LIGHTBAR         0x04
#define F1_PLAYER_LEDS      0x10
/* valid_flag2 */
#define F2_VIBRATION_V2     0x04

/* centre lamp only is the DualSense's "player 1" indicator */
#define PLED_CENTRE 0x04
light_t g_light = { 0, 90, 255, true, LP_SOLID, 0.5, 1.0, LM_MANUAL, PLED_CENTRE };

/* what actually goes on the wire this frame - the pattern writes here so the
 * user's chosen base colour is never overwritten by the animation */
static unsigned char g_eff_r, g_eff_g, g_eff_b;
trigger_fx_t g_fx[2] = { { TFX_OFF, 0, 0, 0 }, { TFX_OFF, 0, 0, 0 } };

static uint32_t crc32_le(const unsigned char *d, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

/*
 * Build one trigger effect block.
 *
 * This used to emit the "simple" effects 0x01 / 0x02 / 0x06, which take raw
 * bytes the firmware does not validate. That is why the force felt uncalibrated:
 * the trigger has ten discrete resistance ZONES with eight strength levels each,
 * so a 0-255 byte was never a force - most of the slider did nothing and the
 * rest saturated.
 *
 * These are the official effects instead:
 *   0x21 Feedback  - zones bitmask + 3 bits of strength per zone
 *   0x25 Weapon    - one start zone and one stop zone, single strength
 *   0x26 Vibration - zones bitmask + per-zone amplitude + a frequency in Hz
 *
 * The stored parameters stay 0-255 so saved configs keep working; they are
 * quantised to the hardware's real resolution here.
 */
#define ZONES 10

static int to_zone(unsigned char v)  { return v * (ZONES - 1) / 255; }        /* 0..9 */
static int to_level(unsigned char v) { return 1 + (int)v * 7 / 255; }         /* 1..8 */

/* Mark every zone from `from` onward, each at `level`. */
static void pack_zones(unsigned char *p, int from, int level) {
    uint16_t active = 0;
    uint32_t packed = 0;
    for (int z = from; z < ZONES; z++) {
        active |= (uint16_t)(1u << z);
        packed |= (uint32_t)((level - 1) & 0x07) << (3 * z);
    }
    p[1] = (unsigned char)(active & 0xFF);
    p[2] = (unsigned char)((active >> 8) & 0xFF);
    p[3] = (unsigned char)(packed & 0xFF);
    p[4] = (unsigned char)((packed >> 8) & 0xFF);
    p[5] = (unsigned char)((packed >> 16) & 0xFF);
    p[6] = (unsigned char)((packed >> 24) & 0xFF);
}

static void put_effect(unsigned char *p, const trigger_fx_t *fx) {
    memset(p, 0, 11);
    switch (fx->mode) {
    case TFX_RESIST: {
        p[0] = 0x21;
        pack_zones(p, to_zone(fx->start), to_level(fx->strength));
        break;
    }
    case TFX_WEAPON: {
        int start = to_zone(fx->start);
        int stop  = to_zone(fx->end);
        if (start < 2) start = 2;                 /* firmware ignores zone 0-1 here */
        if (stop <= start) stop = start + 1;
        if (stop > 8) stop = 8;
        uint16_t marks = (uint16_t)((1u << start) | (1u << stop));
        p[0] = 0x25;
        p[1] = (unsigned char)(marks & 0xFF);
        p[2] = (unsigned char)((marks >> 8) & 0xFF);
        p[3] = (unsigned char)(to_level(fx->strength) - 1);
        break;
    }
    case TFX_VIBRATE: {
        if (fx->end == 0) { p[0] = 0x00; break; }  /* zero Hz means no effect */
        p[0] = 0x26;
        pack_zones(p, to_zone(fx->start), to_level(fx->strength));
        p[9] = fx->end;                            /* frequency, Hz */
        break;
    }
    default:
        p[0] = 0x00;
        break;
    }
}

void out_send(void) {
    IOHIDDeviceRef dev = g_dev;
    if (!dev) return;

    static unsigned char seq = 0;
    unsigned char rep[78];
    memset(rep, 0, sizeof rep);
    rep[0] = 0x31;
    rep[1] = (unsigned char)((seq++ & 0x0F) << 4);
    rep[2] = 0x10;

    pthread_mutex_lock(&g_lock);
    rep[OFF_VALID0] = F0_TRIGGER_R | F0_TRIGGER_L;
    /* Two mutually exclusive rumble paths. v2 goes through valid_flag2 and is
     * meant to be the newer, stronger route, but it is firmware-gated - on this
     * controller it produces no rumble at all, so the classic flag0 path is the
     * default. Never set both. */
    if (g_rumble_v2) rep[OFF_VALID2] |= F2_VIBRATION_V2;
    else             rep[OFF_VALID0] |= F0_COMPAT_VIBRATION | F0_HAPTICS_SELECT;
    rep[OFF_RUMBLE_L] = g_rumble.left;
    rep[OFF_RUMBLE_R] = g_rumble.right;
    rep[OFF_VALID1] = F1_LIGHTBAR | F1_PLAYER_LEDS;
    /* deliberately NOT touching valid_flag2 / lightbar_setup: setup value 0x02
     * is LIGHT_OUT, which kills the bar even while an RGB value is supplied */
    rep[OFF_PLAYER] = g_light.on ? g_light.player : 0;
    rep[OFF_RED]   = g_light.on ? g_eff_r : 0;
    rep[OFF_GREEN] = g_light.on ? g_eff_g : 0;
    rep[OFF_BLUE]  = g_light.on ? g_eff_b : 0;
    put_effect(rep + OFF_TRIG_L, &g_fx[0]);
    put_effect(rep + OFF_TRIG_R, &g_fx[1]);
    pthread_mutex_unlock(&g_lock);

    unsigned char seed = 0xA2;
    uint32_t crc = crc32_le(&seed, 1, 0xFFFFFFFFu);
    crc = crc32_le(rep, 74, crc) ^ 0xFFFFFFFFu;
    rep[74] = (unsigned char)(crc & 0xFF);
    rep[75] = (unsigned char)((crc >> 8) & 0xFF);
    rep[76] = (unsigned char)((crc >> 16) & 0xFF);
    rep[77] = (unsigned char)((crc >> 24) & 0xFF);

    IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x31, rep, sizeof rep);
}

/*
 * Everything that changes output state calls out_mark(); only the run loop
 * actually transmits.
 *
 * Dragging a slider used to fire a synchronous IOHIDDeviceSetReport per HTTP
 * request from the server thread, while the run loop was independently sending
 * on a timer. Two threads pushing blocking writes down one Bluetooth link is
 * what made the motors lag behind the slider. Now writes are coalesced to at
 * most one per frame, from one thread.
 */
static volatile bool g_out_dirty = true;

void out_mark(void) { g_out_dirty = true; }

static void hsv(double h, double s, double v, unsigned char *R,
                unsigned char *G, unsigned char *B) {
    h = fmod(h, 1.0) * 6.0;
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

/* Recompute the effective colour. Patterns are refreshed at 20 Hz rather than
 * every frame - the bar cannot resolve faster and it keeps the Bluetooth link
 * free for input. */
/*
 * Sim mode: the bar becomes an annunciator. Ordered by urgency, because only one
 * thing can be shown at a time and the worst news should win.
 */
static void light_sim(int *pat, unsigned char *r, unsigned char *g, unsigned char *b,
                      double *rate) {
    if (!g_wx.live) {                       /* no telemetry: dim amber standby */
        *pat = LP_BREATHE; *r = 120; *g = 70; *b = 0; *rate = 0.3;
        snprintf(g_lnote, sizeof g_lnote, "waiting for X-Plane");
        return;
    }
    if (g_wx.crashed) {
        *pat = LP_STROBE;  *r = 255; *g = 0; *b = 0; *rate = 3.0;
        snprintf(g_lnote, sizeof g_lnote, "CRASH");
    } else if (g_wx.stall_ratio > 0.6) {
        *pat = LP_PULSE;   *r = 255; *g = 0; *b = 0; *rate = 2.5;
        snprintf(g_lnote, sizeof g_lnote, "stall %.0f%%", g_wx.stall_ratio * 100);
    } else if (g_wx.stall_ratio > 0.15) {
        *pat = LP_BREATHE; *r = 255; *g = 120; *b = 0; *rate = 1.5;
        snprintf(g_lnote, sizeof g_lnote, "stall warning");
    } else if (fabs(g_wx.gn) > 2.5) {
        *pat = LP_SOLID;   *r = 255; *g = 150; *b = 0; *rate = 1.0;
        snprintf(g_lnote, sizeof g_lnote, "%.1f g", g_wx.gn);
    } else if (g_wx.turb > 1.5) {
        *pat = LP_BREATHE; *r = 0; *g = 200; *b = 220; *rate = 1.2;
        snprintf(g_lnote, sizeof g_lnote, "turbulence %.1f", g_wx.turb);
    } else {
        *pat = LP_SOLID;   *r = 0; *g = 190; *b = 90; *rate = 1.0;
        snprintf(g_lnote, sizeof g_lnote, "clean");
    }
}

/*
 * Recompute the emitted colour. Patterns refresh at 20 Hz - the bar cannot
 * resolve faster and it keeps the Bluetooth link free for input.
 */
void light_tick(void) {
    static double last = 0;
    double now = now_sec();

    /* Snapshot under the lock: the HTTP thread writes g_light, and speed and
     * bright are doubles that could otherwise be read half-updated. */
    pthread_mutex_lock(&g_lock);
    int pattern = g_light.pattern;
    int mode = g_light.mode;
    unsigned char br = g_light.r, bg = g_light.g, bb = g_light.b;
    double rate = 0.25 + 2.25 * g_light.speed;
    double k0 = g_light.bright;
    pthread_mutex_unlock(&g_lock);

    /* sim mode overrides colour, pattern and rate; manual mode uses the picker */
    if (mode == LM_SIM) light_sim(&pattern, &br, &bg, &bb, &rate);
    else snprintf(g_lnote, sizeof g_lnote, "manual");

    if (k0 < 0) k0 = 0;
    if (k0 > 1) k0 = 1;

    unsigned char r = br, g = bg, b = bb;

    if (pattern != LP_SOLID) {
        if (now - last < 1.0 / 20.0) return;
        last = now;
        double ph = fmod(now * rate, 1.0);
        switch (pattern) {
        case LP_BREATHE: {                          /* smooth sine fade */
            double k = 0.12 + 0.88 * (0.5 - 0.5 * cos(ph * 2 * M_PI));
            r = (unsigned char)(br * k);
            g = (unsigned char)(bg * k);
            b = (unsigned char)(bb * k);
            break;
        }
        case LP_PULSE: {                            /* quick rise, slow decay */
            double k = ph < 0.15 ? ph / 0.15 : 1.0 - (ph - 0.15) / 0.85;
            r = (unsigned char)(br * k);
            g = (unsigned char)(bg * k);
            b = (unsigned char)(bb * k);
            break;
        }
        case LP_STROBE:                             /* hard on/off */
            if (ph > 0.25) { r = g = b = 0; }
            break;
        case LP_RAINBOW:
            hsv(ph, 1.0, 1.0, &r, &g, &b);
            break;
        case LP_ALERT:                              /* base colour vs amber */
            if (ph > 0.5) { r = 255; g = 120; b = 0; }
            break;
        default: break;
        }
    }

    r = (unsigned char)(r * k0);
    g = (unsigned char)(g * k0);
    b = (unsigned char)(b * k0);
    if (r != g_eff_r || g != g_eff_g || b != g_eff_b) {
        g_eff_r = r; g_eff_g = g; g_eff_b = b;
        out_mark();
    }
}

/* Leave the controller quiet on the way out: no resistance, no buzzing, bar off.
 * Sent synchronously because the run loop is already stopping. */
void out_quiet(void) {
    pthread_mutex_lock(&g_lock);
    g_fx[0] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
    g_fx[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
    g_rumble.left = g_rumble.right = 0;
    g_eff_r = g_eff_g = g_eff_b = 0;
    g_light.player = 0;
    pthread_mutex_unlock(&g_lock);
    out_send();
}

void out_tick(void) {
    static double last_send = 0, last_keep = 0;
    double now = now_sec();

    if (g_out_dirty && now - last_send >= 1.0 / 60.0) {
        g_out_dirty = false;
        last_send = last_keep = now;
        out_send();
        return;
    }
    /* macOS re-asserts its own light bar colour, so hold ours with a slow
     * keepalive even when nothing changed */
    if (now - last_keep >= 0.5) {
        last_keep = last_send = now;
        out_send();
    }
}
