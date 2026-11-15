/*
 * X-Plane's command catalog. X-Plane ships every command with a human-readable
 * description in Resources/plugins/Commands.txt, formatted as <path>, two or more
 * spaces, then <text>. That file is what makes the mapper searchable in plain
 * English. The file belongs to X-Plane and is read from the user's install at
 * runtime; it is never bundled. Pure.
 *
 * Memory: the catalog is one block holding the records (two pointers each) followed by
 * every path and description packed end to end, sized exactly in a first pass. X-Plane
 * 12's 3,159 commands take about 230 KB this way; fixed 512-byte records for the most
 * there could be (MAX_CMDS) took 4 MB for the life of the app.
 */
#include "core_internal.h"
#include <stdlib.h>
#include <string.h>

/* One line of Commands.txt, split: the path and the description (pointers into `line`).
 * False for a line that holds no command: empty, an embedded NUL, or a path too long to
 * be bound (CMD_PATH_MAX; the longest in X-Plane 12.4 is 50). */
static bool split_line(const char *line, size_t ll, const char **path, size_t *plen,
                       const char **desc, size_t *dlen) {
    if (ll && line[ll - 1] == '\r') ll--;                /* CRLF line ends            */
    if (ll == 0 || memchr(line, 0, ll)) return false;
    /* Command paths never contain spaces, so the path ends at the first space or tab.
     * (Most lines use two or more spaces, but a few - e.g. Commands.txt lines 1672 and
     * 1847 in X-Plane 12.4 - use only one.) */
    size_t p = 0;
    while (p < ll && line[p] != ' ' && line[p] != '\t') p++;
    if (p == 0 || p >= CMD_PATH_MAX) return false;
    size_t d = p;
    while (d < ll && (line[d] == ' ' || line[d] == '\t')) d++;   /* the gap           */
    *path = line; *plen = p;
    *desc = line + d; *dlen = ll - d;
    return true;
}

int commands_parse(const char *text, size_t len, xcmd_t **out) {
    *out = NULL;
    if (!text || !len) return 0;
    /* pass 1: how many commands, and how many bytes their strings need (with NULs) */
    size_t n = 0, bytes = 0, pos = 0, start, ll, plen, dlen;
    const char *path, *desc;
    while (n < MAX_CMDS && text_next_line(text, len, &pos, &start, &ll))
        if (split_line(text + start, ll, &path, &plen, &desc, &dlen)) {
            n++;
            bytes += plen + 1 + dlen + 1;
        }
    if (n == 0) return 0;
    /* pass 2: one block, the records first and the strings after them */
    xcmd_t *cmds = malloc(n * sizeof *cmds + bytes);
    if (!cmds) return 0;
    char *s = (char *)(cmds + n);                        /* where the strings go       */
    size_t k = 0;
    pos = 0;
    while (k < n && text_next_line(text, len, &pos, &start, &ll)) {
        if (!split_line(text + start, ll, &path, &plen, &desc, &dlen)) continue;
        memcpy(s, path, plen); s[plen] = 0;
        cmds[k].path = s;
        s += plen + 1;
        memcpy(s, desc, dlen); s[dlen] = 0;
        cmds[k].desc = s;
        s += dlen + 1;
        k++;
    }
    *out = cmds;                                         /* one free() releases it all */
    return (int)k;
}
