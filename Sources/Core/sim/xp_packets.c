/*
 * X-Plane UDP wire formats (port 49000). Pure.
 *
 *   DREF  "DREF" 0x00, float32 value, 500-byte NUL-padded path        = 509 bytes
 *   CMND  "CMND" 0x00, command path, NUL terminator                   = 5 + n + 1
 *   RREF  "RREF" 0x00, int32 frequency, int32 id, 400-byte path        = 413 bytes
 *         X-Plane answers with "RREF" + 1 byte, then {int32 id, float} pairs.
 *         Frequency 0 cancels a subscription.
 * All numbers little-endian (native on every Mac this builds for).
 */
#include "core_internal.h"
#include <string.h>

size_t xp_build_dref(unsigned char *out, const char *path, float value) {
    memset(out, 0, DREF_LEN);
    memcpy(out, "DREF", 4);
    memcpy(out + 5, &value, 4);
    size_t n = strlen(path);
    if (n > 499) n = 499;               /* keep at least one NUL in the field */
    memcpy(out + 9, path, n);
    return DREF_LEN;
}

size_t xp_build_cmnd(unsigned char *out, const char *cmd) {
    /* Laminar's "Exchanging Data with X-Plane" asks for NUL-terminated strings */
    size_t n = strlen(cmd);
    if (n > CMND_MAX - 6) n = CMND_MAX - 6;
    memset(out, 0, CMND_MAX);
    memcpy(out, "CMND", 4);
    memcpy(out + 5, cmd, n);
    return 5 + n + 1;
}

size_t xp_build_rref(unsigned char *out, int32_t freq, int32_t id, const char *path) {
    memset(out, 0, RREF_LEN);
    memcpy(out, "RREF", 4);
    memcpy(out + 5, &freq, 4);
    memcpy(out + 9, &id, 4);
    size_t n = strlen(path);
    if (n > 399) n = 399;
    memcpy(out + 13, path, n);
    return RREF_LEN;
}

int telemetry_parse(const unsigned char *buf, size_t n, rref_pair_t *pairs, int max) {
    if (!buf || n < 13 || memcmp(buf, "RREF", 4) != 0) return 0;
    int count = 0;
    for (size_t o = 5; o + 8 <= n && count < max; o += 8) {
        memcpy(&pairs[count].id, buf + o, 4);
        memcpy(&pairs[count].value, buf + o + 4, 4);
        count++;
    }
    return count;
}
