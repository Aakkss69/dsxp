/* The human-readable vocabulary the UI presents. Control names are labels
 * (sentence-style); axis targets are pop-up menu items (title-style), following
 * Apple's capitalization guidance. */
#include "core_internal.h"

const char *BTN_NAME[B_COUNT] = {
    "D-pad up","D-pad up-right","D-pad right","D-pad down-right",
    "D-pad down","D-pad down-left","D-pad left","D-pad up-left",
    "Square","Cross","Circle","Triangle",
    "L1","R1","L2 click","R2 click",
    "Create","Options","L3 (left stick click)","R3 (right stick click)",
    "PS","Touchpad click","Mute",
    "Tap","Two-finger tap",
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
    "None",
    "Roll (Aileron)",
    "Pitch (Elevator)",
    "Yaw (Rudder)",
    "Throttle",
    "Left Rudder (Differential)",
    "Right Rudder (Differential)",
    "Left Toe Brake",
    "Right Toe Brake"
};
