/*
 * Writing TO the DualSense: light bar, player LEDs, rumble, adaptive triggers.
 * Pure: builds the report; the I/O layer sends it.
 *
 * A plain userspace process can drive the device over Bluetooth even while
 * Apple's AppleGCHIDUserEventDriver holds it (verified 2026-08-26).
 *
 * Bluetooth output report 0x31, 78 bytes:
 *   [0]      report id 0x31
 *   [1]      seq_tag (high nibble)
 *   [2]      tag 0x10
 *   [3..]    common block
 *   [74..77] CRC-32 over 0xA2 + report[0..73], poly 0xEDB88320, little endian
 *
 * Get the CRC wrong and the controller silently discards the packet.
 */
#include "core_internal.h"
#include <string.h>

#define COMMON       3
#define OFF_VALID0   (COMMON + 0)
#define OFF_VALID1   (COMMON + 1)
#define OFF_RUMBLE_R (COMMON + 2)
#define OFF_RUMBLE_L (COMMON + 3)
/* reserved2 in the Linux struct is really the two trigger effect blocks */
#define OFF_TRIG_R   (COMMON + 10)   /* 11 bytes: mode + 10 params */
#define OFF_TRIG_L   (COMMON + 21)
#define OFF_VALID2   (COMMON + 38)
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

uint32_t crc32_le(const unsigned char *d, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

/*
 * Trigger effects. The trigger has ten discrete resistance ZONES with eight
 * strength levels each, so these use Sony's official effects:
 *   0x21 Feedback  - zones bitmask + 3 bits of strength per zone
 *   0x25 Weapon    - one start zone and one stop zone, single strength
 *   0x26 Vibration - zones bitmask + per-zone amplitude + a frequency in Hz
 * Stored parameters stay 0-255 so saved configs keep working; they are
 * quantised to the hardware's real resolution here.
 */
#define ZONES 10

int to_zone(unsigned char v)  { return v * (ZONES - 1) / 255; }        /* 0..9 */
int to_level(unsigned char v) { return 1 + (int)v * 7 / 255; }         /* 1..8 */

/* Mark every zone from `from` onward, each at `level`. */
void pack_zones(unsigned char *p, int from, int level) {
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

void put_effect(unsigned char *p, const trigger_fx_t *fx) {
    memset(p, 0, 11);
    switch (fx->mode) {
    case TFX_RESIST:
        p[0] = 0x21;
        pack_zones(p, to_zone(fx->start), to_level(fx->strength));
        break;
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
    case TFX_VIBRATE:
        if (fx->end == 0) { p[0] = 0x00; break; }  /* zero Hz means no effect */
        p[0] = 0x26;
        pack_zones(p, to_zone(fx->start), to_level(fx->strength));
        p[9] = fx->end;                            /* frequency, Hz */
        break;
    default:
        p[0] = 0x00;
        break;
    }
}

void out_build_report(const out_frame_t *f, unsigned char seq, unsigned char *rep) {
    memset(rep, 0, OUT_REPORT_LEN);
    rep[0] = 0x31;
    rep[1] = (unsigned char)((seq & 0x0F) << 4);
    rep[2] = 0x10;

    rep[OFF_VALID0] = F0_TRIGGER_R | F0_TRIGGER_L;
    /* Two mutually exclusive rumble paths. v2 goes through valid_flag2 and is
     * firmware-gated - on the author's controller it produces no rumble at all,
     * so the classic flag0 path is the default. Never set both. */
    if (f->rumble_v2) rep[OFF_VALID2] |= F2_VIBRATION_V2;
    else              rep[OFF_VALID0] |= F0_COMPAT_VIBRATION | F0_HAPTICS_SELECT;
    rep[OFF_RUMBLE_L] = f->rumble_l;
    rep[OFF_RUMBLE_R] = f->rumble_r;
    rep[OFF_VALID1] = F1_LIGHTBAR | F1_PLAYER_LEDS;
    /* deliberately NOT touching lightbar_setup: value 0x02 is LIGHT_OUT, which
     * kills the bar even while an RGB value is supplied */
    rep[OFF_PLAYER] = f->light_on ? f->player : 0;
    rep[OFF_RED]    = f->light_on ? f->r : 0;
    rep[OFF_GREEN]  = f->light_on ? f->g : 0;
    rep[OFF_BLUE]   = f->light_on ? f->b : 0;
    put_effect(rep + OFF_TRIG_L, &f->fx[0]);
    put_effect(rep + OFF_TRIG_R, &f->fx[1]);

    unsigned char seed = 0xA2;
    uint32_t crc = crc32_le(&seed, 1, 0xFFFFFFFFu);
    crc = crc32_le(rep, 74, crc) ^ 0xFFFFFFFFu;
    rep[74] = (unsigned char)(crc & 0xFF);
    rep[75] = (unsigned char)((crc >> 8) & 0xFF);
    rep[76] = (unsigned char)((crc >> 16) & 0xFF);
    rep[77] = (unsigned char)((crc >> 24) & 0xFF);
}
