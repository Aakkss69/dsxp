/* Is X-Plane 12 running? Process scan for the activation gate. */
#include "core_internal.h"
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XP_EXE_SUFFIX "/X-Plane.app/Contents/MacOS/X-Plane"

/* X-Plane 11 has the same executable path and is not supported, so the version
 * comes from the app bundle: CFBundleShortVersionString, e.g. "12.4.4-b3-5bfd5a53"
 * in X-Plane 12.4.4 beta 3. Any folder name works (F9). */
bool gate_version_ok(const char *exe_path) {
    size_t n = strlen(exe_path), sl = strlen(XP_EXE_SUFFIX);
    if (n <= sl || strcmp(exe_path + n - sl, XP_EXE_SUFFIX) != 0) return false;
    char app[PROC_PIDPATHINFO_MAXSIZE];
    size_t an = n - sl + strlen("/X-Plane.app");
    if (an >= sizeof app) return false;
    memcpy(app, exe_path, an);
    app[an] = 0;

    char plist[PROC_PIDPATHINFO_MAXSIZE + 32];
    snprintf(plist, sizeof plist, "%s/Contents/Info.plist", app);
    size_t len = 0;
    char *data = file_read_all(plist, 1024 * 1024, &len);
    if (!data) return false;
    bool ok = false;
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)data, (CFIndex)len);
    free(data);
    CFPropertyListRef pl = d ? CFPropertyListCreateWithData(kCFAllocatorDefault, d,
                                                            kCFPropertyListImmutable, NULL, NULL) : NULL;
    if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
        CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)pl, CFSTR("CFBundleShortVersionString"));
        char ver[64];
        if (v && CFGetTypeID(v) == CFStringGetTypeID() &&
            CFStringGetCString((CFStringRef)v, ver, sizeof ver, kCFStringEncodingUTF8))
            ok = strtol(ver, NULL, 10) >= 12;          /* major version */
    }
    if (pl) CFRelease(pl);
    if (d) CFRelease(d);
    return ok;
}

bool gate_xplane_running(char *exe_out, size_t n) {
    /* the version check reads a file: remember the answer per executable path */
    static char checked[PROC_PIDPATHINFO_MAXSIZE];
    static bool checked_ok;

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
    size_t sl = strlen(XP_EXE_SUFFIX);
    for (int i = 0; i < count && !found; i++) {
        if (pids[i] == 0) continue;
        if (proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        /* the sim's own executable only: the installer and updater live in the
         * same folder but are other executables */
        size_t pn = strlen(path);
        if (pn <= sl || strcmp(path + pn - sl, XP_EXE_SUFFIX) != 0) continue;
        if (strcmp(path, checked) != 0) {
            checked_ok = gate_version_ok(path);
            snprintf(checked, sizeof checked, "%s", path);
            if (!checked_ok) dsxp_log("%s is not X-Plane 12 or later; ignored", path);
        }
        if (checked_ok) {
            found = true;
            if (n) snprintf(exe_out, n, "%s", path);
        }
    }
    free(pids);
    return found;
}
