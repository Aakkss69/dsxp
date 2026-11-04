/* Shared state and the human-readable vocabulary the UI presents. */
#include "dsxp.h"
#include <CoreFoundation/CoreFoundation.h>

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

binding_t  g_bind[B_COUNT];
axis_map_t g_axis[S_COUNT];
tuning_t   g_tune = { 0.06, 1.00, 0.5 };
ds_state_t g_state;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

bool g_have_report = false, g_engaged = false, g_verbose = false;
bool g_bt_present = false, g_usb_present = false, g_xp_running = false;
long g_reports = 0;
long g_reports_total = 0;
volatile int g_stop = 0;
char g_dir[PATH_MAX];

IOHIDDeviceRef g_dev = NULL;
fxmode_t g_fxmode = FXM_MANUAL;
rmode_t  g_rmode  = RM_MANUAL;
rumble_t g_rumble = { 0, 0 };
char     g_rnote[64] = "manual";
char     g_lnote[64] = "manual";
bool     g_rumble_v2 = false;  /* v2 produced no rumble at all on this
                               * controller's firmware; the classic path works */
xcmd_t *g_cmds = NULL;
int     g_ncmds = 0;

double now_sec(void) { return (double)CFAbsoluteTimeGetCurrent(); }
