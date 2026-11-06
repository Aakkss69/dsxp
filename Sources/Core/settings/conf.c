/*
 * Reading and writing the config files. Pure apart from the locale handle.
 *
 * Format: one "key = value [unit]" per line; '#' starts a comment at the start
 * of a line, or after whitespace inside a value (never at its first character,
 * so "#005AFF" is a color, not a comment). A UTF-8 BOM and CRLF line ends are
 * accepted; keys, choices and yes/no are case-insensitive. Numbers always use
 * '.' whatever the Mac's language (C locale), so a file written on one Mac reads
 * the same on any other.
 *
 * Problems never stop the file from loading: a missing key takes its default,
 * an unusable value keeps the previous one, an out-of-range value is clamped,
 * and each case leaves a warning with its line number.
 */
#include "core_internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xlocale.h>

static locale_t g_cloc;
static pthread_once_t g_cloc_once = PTHREAD_ONCE_INIT;
static void cloc_init(void) { g_cloc = newlocale(LC_ALL_MASK, "C", NULL); }
static locale_t cloc(void) { pthread_once(&g_cloc_once, cloc_init); return g_cloc; }

#define FIELD(c, k, T)  ((T *)((char *)(c) + (k)->off))
#define CFIELD(c, k, T) ((const T *)((const char *)(c) + (k)->off))

static const char *const LAMP_NAME[] = { "off", "single", "two", "three", "four", "all" };
static const unsigned char LAMP_MASK[] = { 0x00, 0x04, 0x0A, 0x0E, 0x1B, 0x1F };

void conf_warn(conf_warns_t *w, int line, const char *key, const char *fmt, ...) {
    if (!w) return;
    if (w->n >= CONF_WARN_MAX) { w->dropped++; return; }
    conf_warn_t *x = &w->w[w->n++];
    x->line = line;
    snprintf(x->key, sizeof x->key, "%s", key ? key : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->msg, sizeof x->msg, fmt, ap);
    va_end(ap);
}

static size_t key_size(const conf_key_t *k) {
    switch (k->type) {
    case CT_INT: case CT_ENUM:              return sizeof(int);
    case CT_U8: case CT_PCTB: case CT_LAMPS: return 1;
    case CT_REAL: case CT_PCT: case CT_TIME: return sizeof(double);
    case CT_BOOL: case CT_ENUMB:            return sizeof(bool);
    case CT_COLOR:                          return 3;
    case CT_BUTTON:                         return sizeof(binding_t);
    case CT_AXIS:                           return sizeof(axis_map_t);
    default:                                return 0;
    }
}

void conf_copy_key(config_t *dst, const config_t *src, const conf_key_t *k) {
    size_t n = key_size(k);
    if (n) memcpy((char *)dst + k->off, (const char *)src + k->off, n);
}

void config_copy_file(config_t *dst, const config_t *src, conf_file_t f) {
    const conf_schema_t *sc = conf_schema(f);
    for (int i = 0; i < sc->nkeys; i++) conf_copy_key(dst, src, &sc->keys[i]);
}

const conf_key_t *conf_find(conf_file_t f, const char *key) {
    const conf_schema_t *sc = conf_schema(f);
    for (int i = 0; i < sc->nkeys; i++)
        if (sc->keys[i].type != CT_SECTION && !strcasecmp(sc->keys[i].key, key)) return &sc->keys[i];
    return NULL;
}

/* ------------------------------------------------------------- numbers */

static bool is_ms(const conf_key_t *k) { return !strcmp(k->unit, "ms"); }

/* stored value -> the number written in the file */
static double to_display(const conf_key_t *k, const config_t *c) {
    switch (k->type) {
    case CT_INT:  return *CFIELD(c, k, int);
    case CT_U8:   return *CFIELD(c, k, unsigned char);
    case CT_REAL: return *CFIELD(c, k, double);
    case CT_PCT:  return *CFIELD(c, k, double) * 100.0;
    case CT_PCTB: return *CFIELD(c, k, unsigned char) * 100.0 / 255.0;
    case CT_TIME: return *CFIELD(c, k, double) * (is_ms(k) ? 1000.0 : 1.0);
    default:      return 0;
    }
}

static void from_display(const conf_key_t *k, config_t *c, double v) {
    switch (k->type) {
    case CT_INT:  *FIELD(c, k, int) = (int)lround(v); break;
    case CT_U8:   *FIELD(c, k, unsigned char) = (unsigned char)lround(v); break;
    case CT_REAL: *FIELD(c, k, double) = v; break;
    case CT_PCT:  *FIELD(c, k, double) = v / 100.0; break;
    case CT_PCTB: *FIELD(c, k, unsigned char) = (unsigned char)lround(v * 255.0 / 100.0); break;
    case CT_TIME: *FIELD(c, k, double) = v / (is_ms(k) ? 1000.0 : 1.0); break;
    default: break;
    }
}

static bool is_numeric(const conf_key_t *k) {
    return k->type == CT_INT || k->type == CT_U8 || k->type == CT_REAL ||
           k->type == CT_PCT || k->type == CT_PCTB || k->type == CT_TIME;
}

static size_t fmt_num(char *out, size_t n, double v, int decimals, const char *unit) {
    int w = unit && *unit ? snprintf_l(out, n, cloc(), "%.*f %s", decimals, v, unit)
                          : snprintf_l(out, n, cloc(), "%.*f", decimals, v);
    return w < 0 ? 0 : (size_t)w;
}

size_t conf_format_value(const conf_key_t *k, const config_t *c, char *out, size_t n) {
    if (n == 0) return 0;
    out[0] = 0;
    int w = 0;
    if (is_numeric(k)) return fmt_num(out, n, to_display(k, c), k->decimals, k->unit);
    switch (k->type) {
    case CT_BOOL:  w = snprintf(out, n, "%s", *CFIELD(c, k, bool) ? "yes" : "no"); break;
    case CT_ENUM: {
        int v = *CFIELD(c, k, int);
        w = snprintf(out, n, "%s", (v >= 0 && v < k->nnames) ? k->names[v] : k->names[0]);
        break;
    }
    case CT_ENUMB: w = snprintf(out, n, "%s", k->names[*CFIELD(c, k, bool) ? 1 : 0]); break;
    case CT_COLOR: {
        const unsigned char *rgb = CFIELD(c, k, unsigned char);
        w = snprintf(out, n, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
        break;
    }
    case CT_LAMPS: {
        unsigned char m = *CFIELD(c, k, unsigned char) & 0x1F;
        for (size_t i = 0; i < sizeof LAMP_MASK; i++)
            if (LAMP_MASK[i] == m) return (size_t)snprintf(out, n, "%s", LAMP_NAME[i]);
        char p[6];
        for (int i = 0; i < 5; i++) p[i] = (m >> i) & 1 ? 'x' : '.';
        p[5] = 0;
        w = snprintf(out, n, "%s", p);
        break;
    }
    case CT_BUTTON: {
        const binding_t *b = CFIELD(c, k, binding_t);
        w = b->cmd[0] ? snprintf(out, n, "%s %s", b->cmd, b->repeat ? "hold" : "once")
                      : snprintf(out, n, "none");
        break;
    }
    case CT_AXIS: {
        const axis_map_t *a = CFIELD(c, k, axis_map_t);
        int t = (a->target >= 0 && a->target < AX_TARGET_COUNT) ? a->target : AX_NONE;
        w = snprintf(out, n, "%s%s", AX_KEY[t], a->invert ? " inverted" : "");
        break;
    }
    default: break;
    }
    return w < 0 ? 0 : (size_t)w;
}

/* ---------------------------------------------------------------- parse */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static bool set_value(const conf_key_t *k, config_t *c, char *v, int line, conf_warns_t *w) {
    if (is_numeric(k)) {
        char *e;
        errno = 0;
        double num = strtod_l(v, &e, cloc());
        if (e == v || errno == ERANGE || !isfinite(num)) {
            conf_warn(w, line, k->key, "\"%s\" is not a usable number; kept the previous value", v);
            return false;
        }
        char *unit = trim(e);
        if (k->type == CT_TIME) {
            if (!strcasecmp(unit, "ms")) num = num / 1000.0 * (is_ms(k) ? 1000.0 : 1.0);
            else if (!strcasecmp(unit, "s")) num = num * (is_ms(k) ? 1000.0 : 1.0);
            else if (*unit) {
                conf_warn(w, line, k->key, "unit \"%s\" is not s or ms; kept the previous value", unit);
                return false;
            }
        } else if (*unit && strcasecmp(unit, k->unit) != 0) {
            conf_warn(w, line, k->key, "unit \"%s\" should be \"%s\"; kept the previous value",
                      unit, *k->unit ? k->unit : "no unit");
            return false;
        }
        if (num < k->lo || num > k->hi) {
            double cl = num < k->lo ? k->lo : k->hi;
            char a[32], b[32];
            fmt_num(a, sizeof a, num, k->decimals, k->unit);
            fmt_num(b, sizeof b, cl, k->decimals, k->unit);
            conf_warn(w, line, k->key, "%s is out of range; using %s", a, b);
            num = cl;
        }
        from_display(k, c, num);
        return true;
    }

    char buf[1024];
    snprintf(buf, sizeof buf, "%s", v);
    char *words[4];
    int nw = 0;
    {
        char *save = NULL;
        for (char *t = strtok_r(buf, " \t", &save); t && nw < 4; t = strtok_r(NULL, " \t", &save))
            words[nw++] = t;
    }

    switch (k->type) {
    case CT_BOOL: {
        static const char *yes[] = { "yes", "true", "on", "1" }, *no[] = { "no", "false", "off", "0" };
        for (int i = 0; nw == 1 && i < 4; i++) {
            if (!strcasecmp(words[0], yes[i])) { *FIELD(c, k, bool) = true; return true; }
            if (!strcasecmp(words[0], no[i]))  { *FIELD(c, k, bool) = false; return true; }
        }
        conf_warn(w, line, k->key, "\"%s\" should be yes or no; kept the previous value", v);
        return false;
    }
    case CT_ENUM:
    case CT_ENUMB:
        for (int i = 0; nw == 1 && i < k->nnames; i++)
            if (!strcasecmp(words[0], k->names[i])) {
                if (k->type == CT_ENUM) *FIELD(c, k, int) = i;
                else *FIELD(c, k, bool) = i == 1;
                return true;
            }
        {
            char list[160] = "";
            for (int i = 0; i < k->nnames; i++)
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", k->names[i]);
            conf_warn(w, line, k->key, "\"%s\" should be one of: %s; kept the previous value", v, list);
        }
        return false;
    case CT_COLOR: {
        unsigned r, g, b;
        char tail;
        if (nw == 1 && strlen(words[0]) == 7 &&
            sscanf(words[0], "#%2x%2x%2x%c", &r, &g, &b, &tail) == 3) {
            unsigned char *rgb = FIELD(c, k, unsigned char);
            rgb[0] = (unsigned char)r; rgb[1] = (unsigned char)g; rgb[2] = (unsigned char)b;
            return true;
        }
        conf_warn(w, line, k->key, "\"%s\" should be a color like #005AFF; kept the previous value", v);
        return false;
    }
    case CT_LAMPS:
        if (nw == 1) {
            for (size_t i = 0; i < sizeof LAMP_MASK; i++)
                if (!strcasecmp(words[0], LAMP_NAME[i])) { *FIELD(c, k, unsigned char) = LAMP_MASK[i]; return true; }
            if (strlen(words[0]) == 5) {
                unsigned char m = 0;
                bool okp = true;
                for (int i = 0; i < 5; i++) {
                    char ch = (char)tolower((unsigned char)words[0][i]);
                    if (ch == 'x') m |= (unsigned char)(1 << i);
                    else if (ch != '.') okp = false;
                }
                if (okp) { *FIELD(c, k, unsigned char) = m; return true; }
            }
        }
        conf_warn(w, line, k->key, "\"%s\" should be off, single, two, three, four, all or a pattern "
                  "like x...x; kept the previous value", v);
        return false;
    case CT_BUTTON: {
        binding_t *b = FIELD(c, k, binding_t);
        if (nw == 0 || (nw == 1 && (!strcasecmp(words[0], "none") || !strcmp(words[0], "sim/none/none")))) {
            b->cmd[0] = 0; b->repeat = false;
            return true;
        }
        bool rep = false;
        if (nw == 2 && !strcasecmp(words[1], "hold")) rep = true;
        else if (nw == 2 && strcasecmp(words[1], "once") != 0) {
            conf_warn(w, line, k->key, "\"%s\" should be hold or once; kept the previous binding", words[1]);
            return false;
        } else if (nw > 2) {
            conf_warn(w, line, k->key, "expected <command> [hold|once]; kept the previous binding");
            return false;
        }
        if (!cmd_valid(words[0])) {
            conf_warn(w, line, k->key, "\"%s\" is not a valid X-Plane command path; kept the previous binding", words[0]);
            return false;
        }
        snprintf(b->cmd, CMD_PATH_MAX, "%s", words[0]);
        b->repeat = rep;
        return true;
    }
    case CT_AXIS: {
        axis_map_t *a = FIELD(c, k, axis_map_t);
        int t = -1;
        for (int i = 0; nw >= 1 && i < AX_TARGET_COUNT; i++) if (!strcasecmp(words[0], AX_KEY[i])) t = i;
        bool inv = false, okw = t >= 0 && nw <= 2;
        if (okw && nw == 2) {
            if (!strcasecmp(words[1], "inverted")) inv = true;
            else if (strcasecmp(words[1], "normal") != 0) okw = false;
        }
        if (!okw) {
            conf_warn(w, line, k->key, "expected one of none, roll, pitch, yaw, throttle, rudder_left, "
                      "rudder_right, brake_left, brake_right, then optionally inverted; kept the previous value");
            return false;
        }
        a->target = t;
        a->invert = inv;
        return true;
    }
    default:
        return false;
    }
}

/* Keys DSXP itself once wrote and no longer uses. A file that still has one is read
 * without a warning (the user did not add it), and the next save leaves it out. */
static const char *const RETIRED_KEYS[] = {
    "motion.yaw_return",     /* the yaw now holds instead of easing back (motion_yaw.c) */
};

static bool retired_key(const char *key) {
    for (size_t i = 0; i < sizeof RETIRED_KEYS / sizeof RETIRED_KEYS[0]; i++)
        if (!strcasecmp(key, RETIRED_KEYS[i])) return true;   /* keys ignore case (conf_find) */
    return false;
}

bool conf_parse(conf_file_t f, const char *text, size_t len, config_t *io, conf_warns_t *w) {
    const conf_schema_t *sc = conf_schema(f);
    if (!sc || !text) return false;
    static config_t prev, def;              /* too big for the stack of every caller */
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    prev = *io;
    config_defaults(&def);
    config_copy_file(io, &def, f);          /* a key that is not in the file: default */
    bool seen[256] = { false };

    size_t i = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) i = 3;
    int line = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t n = i - start;
        if (i < len) i++;
        line++;
        if (n >= 1024) { conf_warn(w, line, "", "line is too long; ignored"); continue; }
        char buf[1024];
        memcpy(buf, text + start, n);
        buf[n] = 0;
        if (strlen(buf) != n) { conf_warn(w, line, "", "line contains a NUL byte; ignored"); continue; }
        if (n && buf[n - 1] == '\r') buf[--n] = 0;
        char *s = trim(buf);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) { conf_warn(w, line, "", "expected key = value; ignored"); continue; }
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        for (char *p = val + 1; *val && *p; p++)            /* inline comment */
            if (*p == '#' && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
        val = trim(val);

        const conf_key_t *k = conf_find(f, key);
        if (!k && retired_key(key)) continue;               /* no longer used: dropped quietly */
        if (!k) {
            const char *home = NULL;
            for (int g = 0; g < CF_COUNT; g++) if (g != (int)f && conf_find((conf_file_t)g, key)) home = conf_schema((conf_file_t)g)->file;
            if (home) conf_warn(w, line, key, "belongs in %s; ignored here", home);
            else conf_warn(w, line, key, "unknown setting; ignored");
            continue;
        }
        int idx = (int)(k - sc->keys);
        if (idx >= 0 && idx < 256) {
            if (seen[idx]) conf_warn(w, line, k->key, "set more than once; the last one wins");
            seen[idx] = true;
        }
        if (!set_value(k, io, val, line, w)) conf_copy_key(io, &prev, k);
    }
    pthread_mutex_unlock(&lk);
    return true;
}

/* --------------------------------------------------------------- format */

typedef struct { char *b; size_t cap, o; bool full; } out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...) {
    if (o->full) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->b + o->o, o->cap - o->o, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->o) { o->full = true; return; }
    o->o += (size_t)n;
}

static void put_doc(out_t *o, const char *doc, const char *indent) {
    if (!doc) return;
    const char *p = doc;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        put(o, "#%s%.*s\n", indent, (int)n, p);
        p += n + (e ? 1 : 0);
    }
}

static void range_line(out_t *o, const conf_key_t *k, const config_t *def) {
    char d[300];
    conf_format_value(k, def, d, sizeof d);
    if (is_numeric(k)) {
        char a[32], b[32];
        fmt_num(a, sizeof a, k->lo, k->decimals, "");
        fmt_num(b, sizeof b, k->hi, k->decimals, "");
        put(o, "#   Range %s to %s%s%s   Default %s\n", a, b, *k->unit ? " " : "", k->unit, d);
    } else if (k->type == CT_ENUM || k->type == CT_ENUMB) {
        put(o, "#   One of:");
        for (int i = 0; i < k->nnames; i++) put(o, "%s %s", i ? "," : "", k->names[i]);
        put(o, "   Default %s\n", d);
    } else if (k->type == CT_BOOL) {
        put(o, "#   yes or no   Default %s\n", d);
    } else if (k->type == CT_COLOR) {
        put(o, "#   #RRGGBB   Default %s\n", d);
    } else if (k->type == CT_LAMPS) {
        put(o, "#   off, single, two, three, four, all, or a pattern like x...x   Default %s\n", d);
    }
}

size_t conf_format(conf_file_t f, const config_t *c, char *buf, size_t cap) {
    const conf_schema_t *sc = conf_schema(f);
    if (!sc || cap == 0) return 0;
    static config_t def;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    config_defaults(&def);
    out_t o = { buf, cap, 0, false };
    buf[0] = 0;

    put(&o, "# DSXP %s settings (format 1)\n#\n", sc->title);
    put_doc(&o, sc->intro, " ");
    put(&o, "#\n# DSXP applies changes to this file within about 2 seconds while it runs.\n");
    if (sc->rewritten)
        put(&o, "# The mapper window also writes this file: your values are kept, but comments\n"
                "# you add yourself are lost when it does.\n");
    else
        put(&o, "# DSXP creates this file once and never writes it again.\n");
    put(&o, "# A line DSXP cannot use is ignored and listed in the mapper and the menu.\n"
            "# Delete a line to use its default; delete the file to reset all of it.\n");

    for (int i = 0; i < sc->nkeys; i++) {
        const conf_key_t *k = &sc->keys[i];
        char v[300];
        if (k->type == CT_SECTION) {
            put(&o, "\n# ===== %s =====\n", k->key);
            if (k->doc) { put_doc(&o, k->doc, " "); }
            continue;
        }
        conf_format_value(k, c, v, sizeof v);
        if (k->type == CT_BUTTON || k->type == CT_AXIS) {
            char d[300];
            conf_format_value(k, &def, d, sizeof d);
            size_t idx = k->type == CT_BUTTON
                ? (k->off - offsetof(config_t, s.bind)) / sizeof(binding_t)
                : (k->off - offsetof(config_t, s.axis)) / sizeof(axis_map_t);
            const char *name = k->type == CT_BUTTON ? (idx < B_COUNT ? BTN_NAME[idx] : "?")
                                                    : (idx < S_COUNT ? SRC_LABEL[idx] : "?");
            put(&o, "# %s. Default: %s\n%s = %s\n", name, d, k->key, v);
            continue;
        }
        put(&o, "\n");
        put_doc(&o, k->doc, " ");
        range_line(&o, k, &def);
        put(&o, "%s = %s\n", k->key, v);
    }
    pthread_mutex_unlock(&lk);
    if (o.full) { buf[0] = 0; return 0; }
    return o.o;
}

/* ---------------------------------------------------------- merge, checks */

/* Per key: a value the mapper changed since the file was last read (ours differs
 * from base) wins; every other key takes the file's value (theirs). */
void conf_merge(conf_file_t f, const config_t *base, const config_t *ours,
                const config_t *theirs, config_t *out) {
    const conf_schema_t *sc = conf_schema(f);
    static config_t tmp;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    tmp = *ours;
    config_copy_file(&tmp, theirs, f);
    for (int i = 0; i < sc->nkeys; i++) {
        const conf_key_t *k = &sc->keys[i];
        if (k->type == CT_SECTION) continue;
        char a[300], b[300];
        conf_format_value(k, ours, a, sizeof a);
        conf_format_value(k, base, b, sizeof b);
        if (strcmp(a, b) != 0) conf_copy_key(&tmp, ours, k);
    }
    *out = tmp;
    pthread_mutex_unlock(&lk);
}

/* Rules that span keys. The later key is adjusted, with a warning. */
void config_validate(config_t *c, conf_warns_t w[CF_COUNT]) {
    bridge_cfg_t *b = &c->bridge;
    double min_to = 3.0 / b->telemetry_hz;
    if (b->telemetry_timeout_s < min_to) {
        conf_warn(&w[CF_BRIDGE], 0, "telemetry.timeout", "must cover three telemetry periods; using %.1f s", min_to);
        b->telemetry_timeout_s = min_to;
    }
    double min_pulse = 1.5 / b->tick_hz;
    if (c->s.gesture.pulse_s < min_pulse) {
        conf_warn(&w[CF_CONTROLS], 0, "gesture.pulse", "must last 1.5 ticks; using %.0f ms", min_pulse * 1000);
        c->s.gesture.pulse_s = min_pulse;
    }
    if (c->s.gesture.swipe_length_at0 <= c->s.gesture.tap_wander_at0) {
        conf_warn(&w[CF_CONTROLS], 0, "gesture.swipe_length_at_0", "must be longer than tap_wander_at_0");
        c->s.gesture.swipe_length_at0 = c->s.gesture.tap_wander_at0 + 0.01;
    }
    if (c->s.gesture.swipe_length_at100 <= c->s.gesture.tap_wander_at100) {
        conf_warn(&w[CF_CONTROLS], 0, "gesture.swipe_length_at_100", "must be longer than tap_wander_at_100");
        c->s.gesture.swipe_length_at100 = c->s.gesture.tap_wander_at100 + 0.01;
    }
    /* Motion: the full-deflection angle must lie at least 1 deg past the deadzone, or the
     * whole usable range would collapse onto a step (the rule: settings.c motion_full_fix;
     * the parser has already held each angle inside its range). */
    static const char *const FULL_KEY[MOTION_AXES] = {
        "motion.roll.full", "motion.pitch.full", "motion.yaw.full" };
    for (int i = 0; i < MOTION_AXES; i++)
        if (motion_full_fix(&c->s.motion, i))
            conf_warn(&w[CF_CONTROLS], 0, FULL_KEY[i], "must be at least 1 deg above its deadzone; using %.0f deg",
                      c->s.motion.full_deg[i]);
    if (c->s.motion.enabled && !c->s.motion.calibrated) {   /* the calibration comes first */
        conf_warn(&w[CF_CONTROLS], 0, "motion.enabled", "needs the calibration first (Motion control > Calibrate in the mapper); left off");
        c->s.motion.enabled = false;
    }
    if (c->s.rumble.jolt_full_g < c->s.rumble.jolt_start_g + 0.5) {
        conf_warn(&w[CF_FEEDBACK], 0, "rumble.g_jolt_full", "must be at least 0.5 g above g_jolt_start");
        c->s.rumble.jolt_full_g = c->s.rumble.jolt_start_g + 0.5;
    }
    if (c->s.wind.buffet_strength_max < c->s.wind.buffet_strength_min) {
        conf_warn(&w[CF_FEEDBACK], 0, "wind.buffet_strength_max", "must not be below buffet_strength_min");
        c->s.wind.buffet_strength_max = c->s.wind.buffet_strength_min;
    }
    if (c->s.wind.buffet_freq_max_hz < c->s.wind.buffet_freq_min_hz) {
        conf_warn(&w[CF_FEEDBACK], 0, "wind.buffet_frequency_max", "must not be below buffet_frequency_min");
        c->s.wind.buffet_freq_max_hz = c->s.wind.buffet_freq_min_hz;
    }
    if (c->s.light_sim.stall_caution >= c->s.light_sim.stall_alarm) {
        conf_warn(&w[CF_FEEDBACK], 0, "light.sim.stall_caution", "must be below stall_alarm");
        c->s.light_sim.stall_caution = c->s.light_sim.stall_alarm - 0.01;
    }
}

int config_write_default_files(const char *dir) {
    if (!mkdir_p(dir, 0755)) return -1;
    static config_t def;
    config_defaults(&def);
    static char buf[64 * 1024];
    for (int f = 0; f < CF_COUNT; f++) {
        size_t n = conf_format((conf_file_t)f, &def, buf, sizeof buf);
        if (n == 0) return -1;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, conf_schema((conf_file_t)f)->file);
        if (!file_write_atomic(path, buf, n, 0644)) return -1;
    }
    return 0;
}
