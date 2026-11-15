/*
 * The DualSense through IOKit HID: making a manager that matches it, reading a device's
 * transport, and walking the matched devices. Shared by the running bridge
 * (runtime/core_thread.c) and the crash guard (platform/guard.c), which both wrote these
 * out on their own before.
 */
#include "core_internal.h"   /* DS_VID, DS_PID                                            */
#include "hid.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>

IOHIDManagerRef hid_dualsense_manager(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return NULL;
    int vid = DS_VID, pid = DS_PID;                    /* Sony, DualSense              */
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef vn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    if (m && vn && pn) {                               /* match both ids, nothing else */
        CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey), vn);
        CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), pn);
        IOHIDManagerSetDeviceMatching(mgr, m);
    }
    if (vn) CFRelease(vn);
    if (pn) CFRelease(pn);
    if (m) CFRelease(m);
    return mgr;
}

bool hid_transport(IOHIDDeviceRef dev, char *out, size_t n) {
    if (n) out[0] = 0;
    CFTypeRef tr = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));
    if (!tr || CFGetTypeID(tr) != CFStringGetTypeID()) return false;
    return CFStringGetCString((CFStringRef)tr, out, (CFIndex)n, kCFStringEncodingUTF8);
}

void hid_each_device(IOHIDManagerRef mgr, bool (*fn)(IOHIDDeviceRef dev, void *arg), void *arg) {
    if (!mgr) return;
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);      /* holds every device while used */
    if (!devs) return;
    CFIndex n = CFSetGetCount(devs);
    const void **arr = malloc(sizeof(void *) * (size_t)(n ? n : 1));
    if (arr) {
        CFSetGetValues(devs, arr);
        for (CFIndex i = 0; i < n; i++)
            if (!fn((IOHIDDeviceRef)(uintptr_t)arr[i], arg)) break;   /* fn says stop    */
        free(arr);
    }
    CFRelease(devs);
}
