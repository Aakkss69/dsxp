/*
 * X-Plane's command catalog. X-Plane ships every command with a human-readable
 * description in Resources/plugins/Commands.txt, formatted as <path>, two or more
 * spaces, then <text>. That file is what makes the mapper searchable in plain
 * English. The file belongs to X-Plane and is read from the user's install at
 * runtime; it is never bundled. Pure.
 */
#include "core_internal.h"
#include <stdlib.h>
#include <string.h>

int commands_parse(const char *text, size_t len, xcmd_t **out) {
    *out = NULL;
    if (!text || !len) return 0;
    xcmd_t *cmds = calloc(MAX_CMDS, sizeof(xcmd_t));
    if (!cmds) return 0;
    int n = 0;
    size_t i = 0;
    while (i < len && n < MAX_CMDS) {
        size_t start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        size_t ll = i - start;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        if (ll == 0) continue;
        const char *line = text + start;

        /* Command paths never contain spaces, so the path ends at the first space or
         * tab. (Most lines use two or more spaces, but a few - e.g. Commands.txt
         * lines 1672 and 1847 in X-Plane 12.4 - use only one.) */
        size_t plen = ll;
        for (size_t k = 0; k < ll; k++)
            if (line[k] == ' ' || line[k] == '\t') { plen = k; break; }
        if (plen == 0) continue;
        if (plen >= CMD_PATH_MAX) plen = CMD_PATH_MAX - 1;
        memcpy(cmds[n].path, line, plen);
        cmds[n].path[plen] = 0;
        if (memchr(cmds[n].path, 0, plen)) continue;   /* embedded NUL */

        size_t d = plen;
        while (d < ll && (line[d] == ' ' || line[d] == '\t')) d++;
        size_t dlen = ll - d;
        if (dlen >= CMD_DESC_MAX) dlen = CMD_DESC_MAX - 1;
        memcpy(cmds[n].desc, line + d, dlen);
        cmds[n].desc[dlen] = 0;
        n++;
    }
    if (n == 0) { free(cmds); return 0; }
    *out = cmds;
    return n;
}
