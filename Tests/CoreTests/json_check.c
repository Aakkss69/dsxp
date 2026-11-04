/* A strict little JSON validator, so tests can prove /api/state and /api/init
 * always produce parseable output. */
#include "tests.h"
#include <ctype.h>
#include <string.h>

typedef struct { const char *s; size_t n, i; int depth; } jp_t;

static void ws(jp_t *p) { while (p->i < p->n && strchr(" \t\r\n", p->s[p->i])) p->i++; }
static bool value(jp_t *p);

static bool string(jp_t *p) {
    if (p->i >= p->n || p->s[p->i] != '"') return false;
    p->i++;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
            if (p->i >= p->n) return false;
            char e = p->s[p->i++];
            if (e == 'u') {
                for (int k = 0; k < 4; k++)
                    if (p->i >= p->n || !isxdigit((unsigned char)p->s[p->i++])) return false;
            } else if (!strchr("\"\\/bfnrt", e)) return false;
        }
    }
    return false;
}

static bool number(jp_t *p) {
    size_t st = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i])) return false;
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i])) return false;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i])) return false;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
    }
    return p->i > st;
}

static bool lit(jp_t *p, const char *w) {
    size_t n = strlen(w);
    if (p->n - p->i < n || memcmp(p->s + p->i, w, n)) return false;
    p->i += n;
    return true;
}

static bool container(jp_t *p, char open, char close, bool obj) {
    if (++p->depth > 64) return false;
    p->i++;                                   /* open */
    ws(p);
    if (p->i < p->n && p->s[p->i] == close) { p->i++; p->depth--; return true; }
    for (;;) {
        ws(p);
        if (obj) {
            if (!string(p)) return false;
            ws(p);
            if (p->i >= p->n || p->s[p->i++] != ':') return false;
        }
        if (!value(p)) return false;
        ws(p);
        if (p->i >= p->n) return false;
        char c = p->s[p->i++];
        if (c == close) { p->depth--; return true; }
        if (c != ',') return false;
    }
    (void)open;
}

static bool value(jp_t *p) {
    ws(p);
    if (p->i >= p->n) return false;
    char c = p->s[p->i];
    if (c == '{') return container(p, '{', '}', true);
    if (c == '[') return container(p, '[', ']', false);
    if (c == '"') return string(p);
    if (c == 't') return lit(p, "true");
    if (c == 'f') return lit(p, "false");
    if (c == 'n') return lit(p, "null");
    return number(p);
}

bool json_valid(const char *s, size_t n) {
    jp_t p = { s, n, 0, 0 };
    if (!value(&p)) return false;
    ws(&p);
    return p.i == n;
}
