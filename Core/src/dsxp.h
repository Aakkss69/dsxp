/*
 * dsxp - DualSense -> X-Plane 12 bridge for macOS
 *
 * Over Bluetooth the DualSense streams HID input report 0x31 (78 bytes), declared
 * by its descriptor as one opaque vendor blob (UsagePage 0xFF00, Usage 0x3B,
 * ReportSize 8 x ReportCount 77) - no Generic Desktop axes, no Button usages.
 * X-Plane's HID bridge discards every 0xFF00 element ("SKIPPED (ff00/xx) ...
 * because of usage page" in its own Log.txt), so over Bluetooth it discards the
 * whole report and never sees an axis or a button. Over USB the real axes ride in
 * report 0x01 alongside the vendor blobs, so the same filter is harmless.
 *
 * macOS latches the controller into 0x31 mode: AppleGCHIDUserEventDriver
 * (GameControllerCategory "ps5") reads calibration feature report 0x05. Not
 * reversible from userspace.
 *
 * So we read report 0x31 ourselves and write X-Plane's datarefs over UDP.
 */
#ifndef DSXP_H
#define DSXP_H

#include <stdbool.h>
#include <pthread.h>
#include <limits.h>
#include <IOKit/hid/IOHIDManager.h>

/* ------------------------------------------------------------- constants */

#define XP_IP         "127.0.0.1"
#define XP_PORT       49000
#define UI_PORT       8765
#define DS_VID        0x054C
#define DS_PID        0x0CE6
#define BT_REPORT_ID  0x31
#define GATE_POLL_SEC 2.0
#define CMD_REPEAT_HZ 20.0
#define MAX_CMDS      8192
#define CMD_PATH_MAX  256
#define CMD_DESC_MAX  256

/* Bluetooth report 0x31 byte offsets. Per Linux drivers/hid/hid-playstation.c the
 * payload is { x, y, rx, ry, z, rz, seq_number, buttons[4], ... }; USB starts it
 * at data[1], Bluetooth at data[2] (one extra framing byte). Our buffer includes
 * the report-ID byte at index 0. VERIFIED against hardware. */
#define OFF_LX 2
#define OFF_LY 3
#define OFF_RX 4
#define OFF_RY 5
#define OFF_L2 6
#define OFF_R2 7
#define OFF_BTN0 9   /* dpad nibble (bits 0-3) + square/cross/circle/triangle */
#define OFF_BTN1 10  /* L1 R1 L2 R2 Create Options L3 R3                      */
#define OFF_BTN2 11  /* PS Touchpad Mute                                      */
/* Two touch points, 4 bytes each. Payload+32 and +36; Bluetooth payload starts
 * at data[2]. Each point: [0] bit7 set = NOT touching, bits0-6 = touch id;
 * [1] x_lo; [2] low nibble x_hi, high nibble y_lo; [3] y_hi. 1920 x 1080. */
#define OFF_TOUCH0 34
#define OFF_TOUCH1 38
#define TOUCH_W 1920.0
#define TOUCH_H 1080.0

/* X-Plane numbers the 8 hat directions as button slots 0-7 and starts real HID
 * buttons at slot 8 - same convention as the .prf files, so bindings transcribed
 * from a profile line up directly. */
enum {
    B_DP_UP = 0, B_DP_UR, B_DP_R, B_DP_DR, B_DP_DOWN, B_DP_DL, B_DP_L, B_DP_UL,
    B_SQUARE = 8, B_CROSS, B_CIRCLE, B_TRIANGLE,
    B_L1, B_R1, B_L2D, B_R2D,
    B_CREATE, B_OPTIONS, B_L3, B_R3,
    B_PS, B_TOUCHPAD, B_MUTE,
    /* Touchpad gestures. Appended after the physical controls on purpose, so
     * slot numbers 0-22 stay stable and existing bindings.conf files still load. */
    B_TP_TAP = 23, B_TP_TAP2,
    B_TP_SWIPE_L, B_TP_SWIPE_R, B_TP_SWIPE_U, B_TP_SWIPE_D,
    B_TP_SWIPE_UL, B_TP_SWIPE_UR, B_TP_SWIPE_DL, B_TP_SWIPE_DR,
    B_COUNT
};

/* Analog sources */
enum { S_LX = 0, S_LY, S_RX, S_RY, S_L2, S_R2, S_COUNT };

/* What an analog source drives */
enum {
    AX_NONE = 0, AX_ROLL, AX_PITCH, AX_YAW, AX_THROTTLE,
    AX_RUDD_L, AX_RUDD_R, AX_BRAKE_L, AX_BRAKE_R, AX_TARGET_COUNT
};

/* ----------------------------------------------------------------- types */

typedef struct { char cmd[CMD_PATH_MAX]; bool repeat; } binding_t;
typedef struct { int target; bool invert; } axis_map_t;
typedef struct { double deadzone; double curve; double gesture; } tuning_t;
/* Light bar: a static colour, or an animated pattern built from it. */
typedef enum { LP_SOLID = 0, LP_BREATHE, LP_PULSE, LP_STROBE, LP_RAINBOW, LP_ALERT,
               LP_COUNT } lpat_t;
typedef enum { LM_MANUAL = 0, LM_SIM } lmode_t;
typedef struct {
    unsigned char r, g, b;
    bool on;
    int  pattern;          /* lpat_t */
    double speed;          /* 0..1 */
    double bright;         /* 0..1, scales the emitted colour */
    int  mode;             /* lmode_t - sim mode drives colour and pattern */
    unsigned char player;  /* player-indicator LED bitmask, 5 lamps */
} light_t;
typedef enum { TFX_OFF = 0, TFX_RESIST, TFX_WEAPON, TFX_VIBRATE } tfx_mode_t;
typedef struct { int mode; unsigned char start, end, strength; } trigger_fx_t;
typedef struct { bool active; double x, y; } touch_t;

/* Trigger effects are either hand-set or driven from the sim, never both. */
typedef enum { FXM_MANUAL = 0, FXM_WIND } fxmode_t;
typedef enum { RM_MANUAL = 0, RM_SIM } rmode_t;
typedef struct { unsigned char left, right; } rumble_t;

typedef struct {
    double ias;          /* knots indicated                             */
    double wind_ms;      /* effective wind at the aircraft, m/s         */
    double wind_dir;     /* degrees true, the direction it blows FROM   */
    double heading;      /* aircraft true heading, degrees              */
    double turb;         /* 0-10                                        */
    double crosswind;    /* derived, m/s, positive = from the right     */
    double load;         /* derived, 0-1 aerodynamic load               */
    double stall_l;      /* 0-1 fraction of sampled left-wing elements stalled  */
    double stall_r;      /* 0-1 right wing                                      */
    double stall_ratio;  /* 0-1 overall stall warning                           */
    double gn;           /* normal load factor, Gs                              */
    bool   crashed;
    double last_rx;
    bool   live;
    char   note[64];
} wx_t;
typedef struct { char path[CMD_PATH_MAX]; char desc[CMD_DESC_MAX]; } xcmd_t;

typedef struct {
    double raw01[S_COUNT];   /* 0..1 exactly as delivered  */
    double val[S_COUNT];     /* shaped: -1..1, or 0..1 for triggers */
    bool   btn[B_COUNT];
    touch_t touch[2];        /* normalised 0..1 across the pad */
} ds_state_t;

/* ---------------------------------------------------------- shared state */

extern const char *BTN_NAME[B_COUNT];
extern const char *SRC_KEY[S_COUNT];
extern const char *SRC_LABEL[S_COUNT];
extern const char *AX_KEY[AX_TARGET_COUNT];
extern const char *AX_LABEL[AX_TARGET_COUNT];

extern binding_t  g_bind[B_COUNT];
extern axis_map_t g_axis[S_COUNT];
extern tuning_t   g_tune;
extern ds_state_t g_state;
extern pthread_mutex_t g_lock;

extern bool  g_have_report, g_engaged, g_verbose;
extern bool  g_bt_present, g_usb_present, g_xp_running;
extern long  g_reports;        /* rolling 10 s window */
extern long  g_reports_total;  /* lifetime */
extern volatile int g_stop;
extern char  g_dir[PATH_MAX];

extern xcmd_t *g_cmds;
extern int     g_ncmds;

extern light_t      g_light;
extern trigger_fx_t g_fx[2];        /* [0] = L2, [1] = R2 */
extern IOHIDDeviceRef g_dev;        /* the Bluetooth DualSense, for output reports */
extern wx_t     g_wx;
extern fxmode_t g_fxmode;
extern rmode_t  g_rmode;
extern rumble_t g_rumble;
extern char     g_rnote[64];
extern bool     g_rumble_v2;
extern char     g_lnote[64];

double now_sec(void);

/* --------------------------------------------------------------- modules */

/* dualsense.c */
void ds_decode_report(const unsigned char *r, long len);
IOHIDManagerRef ds_open(void);

/* xplane.c */
void xp_udp_open(void);
void xp_send_dref(const char *path, float value);
void xp_send_cmnd(const char *cmd);
void xp_engage(void);
void xp_disengage(void);
void xp_push(void);
bool xp_axis_in_use(int target);
int  xp_socket(void);
const void *xp_addr(void);

/* bindings.c */
void bindings_load(void);
void bindings_save(void);
void commands_load(void);

/* gate.c */
bool gate_xplane_running(void);
void gate_rescan_transports(IOHIDManagerRef mgr);

/* wind.c */
void wind_tick(void);

/* rumble.c */
void rumble_apply(void);

/* output.c */
void out_send(void);
void out_mark(void);
void light_tick(void);
void out_quiet(void);
void out_tick(void);

/* reports.c */
void reports_tick(void);

/* httpd.c */
void *httpd_thread(void *arg);

#endif /* DSXP_H */
