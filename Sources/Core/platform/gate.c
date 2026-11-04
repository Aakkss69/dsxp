/* Is X-Plane 12 running? Process scan for the activation gate. */
#include "core_internal.h"
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gate_xplane_running(char *exe_out, size_t n) {
    if (n) exe_out[0] = 0;
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return false;
    /* headroom for processes started between the two calls */
    size_t cap = (size_t)bytes + 64 * sizeof(pid_t);
    pid_t *pids = malloc(cap);
    if (!pids) return false;
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)cap);
    int count = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;
    bool found = false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < count && !found; i++) {
        if (pids[i] == 0) continue;
        if (proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        /* Match the sim's own executable so the installer and updater, which also
         * live under "X-Plane 12", do not trip the gate. */
        if (strstr(path, "X-Plane 12") && strstr(path, "X-Plane.app/Contents/MacOS/X-Plane")) {
            found = true;
            if (n) snprintf(exe_out, n, "%s", path);
        }
    }
    free(pids);
    return found;
}
