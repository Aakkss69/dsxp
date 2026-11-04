/*
 * The activation gate. All three must hold before anything is sent to X-Plane:
 *   1. a DualSense is present
 *   2. on Bluetooth, and NOT also on USB - the USB path works natively through
 *      X-Plane's own HID handling, so the bridge must stay out of its way
 *   3. X-Plane 12 is running
 */
#include "dsxp.h"
#include <libproc.h>
#include <stdlib.h>
#include <string.h>

bool gate_xplane_running(void) {
    int n = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (n <= 0) return false;
    pid_t *pids = malloc((size_t)n);
    if (!pids) return false;
    n = proc_listpids(PROC_ALL_PIDS, 0, pids, n);
    int count = n / (int)sizeof(pid_t);
    bool found = false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < count && !found; i++) {
        if (pids[i] == 0) continue;
        if (proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        /* Match the sim's own executable so the installer and updater, which also
         * live under "X-Plane 12", do not trip the gate. */
        if (strstr(path, "X-Plane 12") &&
            strstr(path, "X-Plane.app/Contents/MacOS/X-Plane"))
            found = true;
    }
    free(pids);
    return found;
}

void gate_rescan_transports(IOHIDManagerRef mgr) {
    g_bt_present = g_usb_present = false;
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (!devs) return;
    CFIndex n = CFSetGetCount(devs);
    IOHIDDeviceRef *arr = malloc(sizeof(IOHIDDeviceRef) * (size_t)(n ? n : 1));
    if (!arr) { CFRelease(devs); return; }
    CFSetGetValues(devs, (const void **)arr);
    for (CFIndex i = 0; i < n; i++) {
        CFStringRef tr = IOHIDDeviceGetProperty(arr[i], CFSTR(kIOHIDTransportKey));
        if (!tr) continue;
        char tb[64] = "";
        CFStringGetCString(tr, tb, sizeof tb, kCFStringEncodingUTF8);
        if (!strcmp(tb, "Bluetooth")) g_bt_present = true;
        else if (!strcmp(tb, "USB"))  g_usb_present = true;
    }
    free(arr);
    CFRelease(devs);
}
