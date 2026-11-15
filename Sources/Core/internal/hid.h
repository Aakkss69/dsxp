/*
 * The DualSense through IOKit HID (platform/hid.c): the pieces the running bridge
 * (core_thread.c) and the crash guard (guard.c) both need, kept in one place.
 * Not pure: these talk to IOKit, so only the I/O modules include this header.
 */
#ifndef DSXP_HID_H
#define DSXP_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <IOKit/hid/IOHIDManager.h>

/* A HID manager matching the DualSense (DS_VID / DS_PID), not yet opened. NULL on failure;
 * the caller releases it (CFRelease). */
IOHIDManagerRef hid_dualsense_manager(void);

/* The device's transport ("Bluetooth", "USB") into out (n bytes). False when unknown. */
bool hid_transport(IOHIDDeviceRef dev, char *out, size_t n);

/* Call fn for every device the manager has matched, until fn returns false. The devices
 * stay valid during the call only. */
void hid_each_device(IOHIDManagerRef mgr, bool (*fn)(IOHIDDeviceRef dev, void *arg), void *arg);

#endif /* DSXP_HID_H */
