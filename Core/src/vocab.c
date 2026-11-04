/* The human-readable vocabulary the UI presents. */
#include "core_internal.h"

const char *BTN_NAME[B_COUNT] = {
    "D-pad Up","D-pad Up-Right","D-pad Right","D-pad Down-Right",
    "D-pad Down","D-pad Down-Left","D-pad Left","D-pad Up-Left",
    "Square","Cross","Circle","Triangle",
    "L1","R1","L2 (digital)","R2 (digital)",
    "Create","Options","L3 (stick click)","R3 (stick click)",
    "PS","Touchpad","Mute",
    "Touch tap","Touch 2-finger tap",
    "Swipe left","Swipe right","Swipe up","Swipe down",
    "Swipe up-left","Swipe up-right","Swipe down-left","Swipe down-right"
};

const char *SRC_KEY[S_COUNT]   = { "lx","ly","rx","ry","l2","r2" };
const char *SRC_LABEL[S_COUNT] = {
    "Left stick X","Left stick Y","Right stick X","Right stick Y",
    "L2 trigger","R2 trigger"
};

const char *AX_KEY[AX_TARGET_COUNT] = {
    "none","roll","pitch","yaw","throttle","rudder_left","rudder_right",
    "brake_left","brake_right"
};
const char *AX_LABEL[AX_TARGET_COUNT] = {
    "Unassigned",
    "Roll (aileron)",
    "Pitch (elevator)",
    "Yaw (rudder)",
    "Throttle",
    "Rudder left (differential)",
    "Rudder right (differential)",
    "Left toe brake",
    "Right toe brake"
};
