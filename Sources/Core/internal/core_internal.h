/*
 * DSXP core - internal declarations.
 *
 * Why this exists: over Bluetooth the DualSense streams HID input report 0x31
 * (78 bytes), which its descriptor declares as one opaque vendor blob
 * (UsagePage 0xFF00, Usage 0x3B, ReportSize 8 x ReportCount 77) - no Generic
 * Desktop axes, no Button usages. X-Plane's HID bridge discards every 0xFF00
 * element ("SKIPPED (ff00/xx) ... because of usage page" in its Log.txt), so over
 * Bluetooth it never sees an axis or a button. Over USB the real axes ride in
 * report 0x01 alongside the vendor blobs, so the same filter is harmless.
 *
 * macOS latches the controller into 0x31 mode (AppleGCHIDUserEventDriver reads
 * calibration feature report 0x05); that is not reversible from userspace. So the
 * core reads report 0x31 itself and writes X-Plane's datarefs over UDP.
 *
 * Layout of Sources/Core (pure logic is testable and makes no system calls):
 *   include/   public API (dsxp_core.h) - the only header Swift sees
 *   internal/  shared internal headers (this file)
 *   input/     decode.c vocab.c                     pure: report 0x31, shaping, gestures, names
 *   output/    output.c wind.c rumble.c light.c     pure: output report and effect models
 *   sim/       xplane.c telemetry.c xp_packets.c commands.c   pure: X-Plane side
 *   settings/  bindings.c                           pure: defaults and settings text
 *   api/       api.c json.c                         pure: the mapper's JSON API
 *   runtime/   core.c (pure state machine), core_thread.c (threads, run loop, HID, UDP)
 *   platform/  gate.c guard.c fileio.c log.c        macOS services
 */
#ifndef DSXP_CORE_INTERNAL_H
#define DSXP_CORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsxp_core.h"

/* ------------------------------------------------------------- constants */

#define XP_IP         "127.0.0.1"
#define XP_PORT       49000
#define DS_VID        0x054C
#define DS_PID        0x0CE6
#define BT_REPORT_ID  0x31
#define GATE_POLL_SEC 2.0
#define CMD_REPEAT_HZ 20.0
#define MAX_CMDS      8192
#define CMD_PATH_MAX  256
#define CMD_DESC_MAX  256

#define TICK_HZ                 60.0
#define INPUT_STALE_SEC         0.25  /* centre the yoke after this long without input */
#define INPUT_LOST_SEC          2.0   /* treat the controller as gone after this       */
#define OVERRIDE_REASSERT_SEC   1.0
#define OVERRIDE_RELEASE_REPEAT 3
#define THROTTLE_KEEPALIVE_SEC  5.0   /* resend an unchanged throttle this often; 0 = never */
#define TELEMETRY_TIMEOUT_SEC   3.0
#define TELEMETRY_RESUB_SEC     5.0
#define TELEMETRY_HZ            10
#define SAVE_DEBOUNCE_SEC       0.5
#define REPORT_WINDOW_SEC       10.0
#define REPORT_IDLE_LOG_SEC     3600.0
#define GESTURE_PULSE_SEC       0.08
#define OUT_REPORT_LEN          78
#define SETTINGS_MAX_BYTES      (1024 * 1024)

/* Bluetooth report 0x31 byte offsets. Per Linux drivers/hid/hid-playstation.c the
 * payload is { x, y, rx, ry, z, rz, seq_number, buttons[4], ... }; USB starts it
 * at data[1], Bluetooth at data[2] (one extra framing byte). The buffer includes
 * the report-ID byte at index 0. Verified against hardware 2026-08-25. */
#define OFF_LX 2
#define OFF_LY 3
#define OFF_RX 4
#define OFF_RY 5
#define OFF_L2 6
#define OFF_R2 7
#define OFF_BTN0 9   /* dpad nibble (bits 0-3) + square/cross/circle/triangle */
#define OFF_BTN1 10  /* L1 R1 L2 R2 Create Options L3 R3                      */
#define OFF_BTN2 11  /* PS Touchpad Mute                                      */
/* Two touch points, 4 bytes each. Each point: [0] bit7 set = NOT touching,
 * bits0-6 = touch id; [1] x_lo; [2] low nibble x_hi, high nibble y_lo; [3] y_hi.
 * Pad resolution 1920 x 1080. */
#define OFF_TOUCH0 34
#define OFF_TOUCH1 38
#define TOUCH_W 1920.0
#define TOUCH_H 1080.0
#define IN_REPORT_MIN_LEN (OFF_TOUCH1 + 4)   /* 42: every byte we read */

/* X-Plane numbers the 8 hat directions as button slots 0-7 and starts real HID
 * buttons at slot 8 - same convention as its .prf files, so bindings transcribed
 * from a profile line up directly. */
enum {
    B_DP_UP = 0, B_DP_UR, B_DP_R, B_DP_DR, B_DP_DOWN, B_DP_DL, B_DP_L, B_DP_UL,
    B_SQUARE = 8, B_CROSS, B_CIRCLE, B_TRIANGLE,
    B_L1, B_R1, B_L2D, B_R2D,
    B_CREATE, B_OPTIONS, B_L3, B_R3,
    B_PS, B_TOUCHPAD, B_MUTE,
    /* Touchpad gestures, appended after the physical controls so slots 0-22
     * stay stable and existing bindings.conf files still load. */
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
    bool   paused;       /* sim/time/paused                              */
    double elev_m;       /* aircraft elevation MSL, metres                 */
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

/* Gesture recogniser state (was function statics in the prototype). */
typedef struct {
    bool   was;
    bool   saw_second;
    double sx, sy, st;
    double pulse[B_COUNT];   /* a gesture slot reads as pressed until this time */
} gesture_t;

/* Everything that is saved to bindings.conf. */
typedef struct {
    binding_t    bind[B_COUNT];
    axis_map_t   axis[S_COUNT];
    tuning_t     tune;
    light_t      light;
    trigger_fx_t fx_manual[2];  /* [0] = L2, [1] = R2: what the user set by hand */
    int          fxmode;        /* fxmode_t */
    int          rmode;         /* rmode_t  */
    bool         rumble_v2;
} settings_t;

/* X-Plane 12 telemetry (RREF ids). Weather is published per layer: 13 layers of
 * turbulence with their centre altitudes (DataRefs.txt: sim/weather/aircraft/
 * turbulence and wind_altitude_msl_m, both float[13]); DSXP interpolates at the
 * aircraft elevation. Wing elements are float[480] laid out as [48 wings][10
 * elements]; even wings are left, odd wings right. */
#define WX_LAYERS     13
#define STALL_WINGS   4
#define STALL_SAMPLES 3      /* elements 2, 5, 8: mid-span, away from root and tip */
enum { RR_IAS = 1, RR_WSPD, RR_WDIR, RR_HDG, RR_STALLR, RR_CRASH, RR_GN, RR_PAUSED, RR_ELEV,
       RR_TURB0, RR_ALT0 = RR_TURB0 + WX_LAYERS, RR_STALL0 = RR_ALT0 + WX_LAYERS,
       RR_COUNT = RR_STALL0 + STALL_WINGS * STALL_SAMPLES };

/* Override datarefs the bridge may assert. */
enum { OV_PITCH_I = 0, OV_ROLL_I, OV_HEADING_I, OV_BRAKES_I, OV_COUNT };

/* Where the pure core sends things. The I/O layer fills these in; tests capture. */
typedef struct {
    void *ctx;
    void (*udp_send)(void *ctx, const unsigned char *buf, size_t len);  /* to X-Plane */
    void (*out_report)(void *ctx, const unsigned char *rep);           /* 78 bytes    */
    void (*save)(void *ctx, const char *text, size_t len);             /* settings    */
} core_sinks_t;

typedef struct core {
    settings_t   s;
    core_sinks_t sinks;
    bool         verbose;
    bool         dry_run;    /* never engage or talk to X-Plane (diagnostics) */

    /* input */
    ds_state_t state;
    gesture_t  gest;
    bool       have_report;
    double     last_report_at;
    long       reports_window, reports_total;
    double     window_start, report_hz;
    double     last_report_log;
    char       last_log_sig[64];

    /* gate */
    bool   bt_present, usb_present, xp_running;
    char   xp_exe[1024];
    bool   engaged;
    bool   suspended;
    bool   force_gate;

    /* X-Plane push */
    bool   prev_btn[B_COUNT];
    double last_cmd_time[B_COUNT];
    bool   ov_on[OV_COUNT];
    double ov_last[OV_COUNT];
    int    ov_release_left[OV_COUNT];
    double ov_release_at[OV_COUNT];
    double last_thr, last_thr_at;
    bool   thr_sent;

    /* telemetry */
    wx_t   wx;
    float  turb_layer[WX_LAYERS], alt_layer[WX_LAYERS];
    float  stall_el[STALL_WINGS][STALL_SAMPLES];
    bool   subscribed;
    double last_sub;

    /* controller output */
    trigger_fx_t fx_out[2];
    rumble_t     rumble;
    char         rnote[64], lnote[64];
    unsigned char eff_r, eff_g, eff_b;
    double       light_last;
    bool         out_dirty;
    double       last_send, last_keep;
    unsigned char out_seq;
    bool         have_output_device;

    /* persistence */
    bool   cfg_dirty;
    double cfg_dirty_at;
    bool   imported;
} core_t;

/* Snapshot of everything /api/state shows, copied out under a short lock. */
typedef struct {
    bool connected; long reports, total; double report_hz;
    tuning_t tune; light_t light; char lnote[64];
    trigger_fx_t fx[2]; int fxmode;
    wx_t wx; int rmode; char rnote[64]; rumble_t rumble; bool rumble_v2;
    bool bt, usb, xp, engaged, suspended, imported;
    double axes[S_COUNT], raw[S_COUNT];
    bool btn[B_COUNT];
    binding_t bind[B_COUNT];
    axis_map_t axis[S_COUNT];
    touch_t touch[2];
    char xp_exe[1024];
} dsxp_snap_t;

/* Commands from the UI into the core (applied on the core thread). */
typedef enum {
    CMD_NONE = 0, CMD_BIND, CMD_AXIS, CMD_TUNE, CMD_LIGHT, CMD_TRIGGER, CMD_FXMODE,
    CMD_RUMBLE, CMD_RUMBLEMODE, CMD_RUMBLEPATH, CMD_SUSPEND
} cmd_kind_t;

enum { LF_R = 1, LF_G = 2, LF_B = 4, LF_ON = 8, LF_PATTERN = 16, LF_SPEED = 32,
       LF_PLAYER = 64, LF_BRIGHT = 128, LF_MODE = 256 };
enum { TF_MODE = 1, TF_START = 2, TF_END = 4, TF_STRENGTH = 8 };

typedef struct {
    cmd_kind_t kind;
    int key;                                   /* slot / source / trigger side */
    union {
        struct { char cmd[CMD_PATH_MAX]; bool repeat; } bind;
        axis_map_t axis;
        struct { bool has_dz, has_cv, has_gs; double dz, cv, gs; } tune;
        struct { light_t v; unsigned mask; } light;
        struct { trigger_fx_t v; unsigned mask; } trig;
        int mode;
        struct { bool has_l, has_r; unsigned char l, r; } rumble;
        bool flag;
    } u;
} cmd_t;

/* ----------------------------------------------------------- vocabulary */

extern const char *BTN_NAME[B_COUNT];
extern const char *SRC_KEY[S_COUNT];
extern const char *SRC_LABEL[S_COUNT];
extern const char *AX_KEY[AX_TARGET_COUNT];
extern const char *AX_LABEL[AX_TARGET_COUNT];

/* ------------------------------------------------------- pure modules */

/* decode.c */
double shape(double v, double deadzone, double curve);
void   decode_touch(ds_state_t *st, const unsigned char *r);
void   detect_gestures(const ds_state_t *st, gesture_t *g, double sens, double now);
bool   ds_decode_report(ds_state_t *st, gesture_t *g, const tuning_t *tune,
                        const unsigned char *r, long len, double now);

/* output.c - Bluetooth output report 0x31 */
typedef struct {
    unsigned char rumble_l, rumble_r;
    bool          rumble_v2;
    bool          light_on;
    unsigned char player, r, g, b;
    trigger_fx_t  fx[2];          /* [0] = L2, [1] = R2 */
} out_frame_t;
uint32_t crc32_le(const unsigned char *d, size_t n, uint32_t crc);
int      to_zone(unsigned char v);
int      to_level(unsigned char v);
void     pack_zones(unsigned char *p, int from, int level);
void     put_effect(unsigned char *p, const trigger_fx_t *fx);
bool     fx_equal(const trigger_fx_t *a, const trigger_fx_t *b);   /* field-wise: the struct has padding */
void     out_build_report(const out_frame_t *f, unsigned char seq, unsigned char *rep);

/* wind.c / rumble.c / light.c */
void wind_apply(wx_t *wx, trigger_fx_t fx[2]);
void rumble_apply(const wx_t *wx, rumble_t *out, char *note, size_t note_n);
void hsv(double h, double s, double v, unsigned char *R, unsigned char *G, unsigned char *B);
void light_sim(const wx_t *wx, int *pat, unsigned char *r, unsigned char *g,
               unsigned char *b, double *rate, char *note, size_t note_n);
bool light_tick(core_t *c, double now);     /* true if the emitted colour changed */

/* xp_packets.c - X-Plane UDP wire formats */
#define DREF_LEN 509
#define CMND_MAX 505
#define RREF_LEN 413
typedef struct { int32_t id; float value; } rref_pair_t;
size_t xp_build_dref(unsigned char *out, const char *path, float value);
size_t xp_build_cmnd(unsigned char *out, const char *cmd);
size_t xp_build_rref(unsigned char *out, int32_t freq, int32_t id, const char *path);
int    telemetry_parse(const unsigned char *buf, size_t n, rref_pair_t *pairs, int max);

/* bindings.c */
void   load_defaults(settings_t *s);
bool   cmd_valid(const char *cmd);
double clampd(double v, double lo, double hi, double dflt);
int    clampi(long v, int lo, int hi);
bool   bindings_parse(settings_t *s, const char *text, size_t len);
size_t bindings_format(const settings_t *s, char *buf, size_t cap);

/* commands.c */
int    commands_parse(const char *text, size_t len, xcmd_t **out);

/* json.c */
typedef struct { char *b; size_t len, cap; bool oom; } jbuf_t;
void   jb_init(jbuf_t *j, size_t cap);
void   jb_free(jbuf_t *j);
void   jb_printf(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void   jb_str(jbuf_t *j, const char *s);                  /* "escaped" */
size_t json_escape(const char *s, char *out, size_t n);
void   url_decode(const char *in, char *out, size_t n);
const char *qs_get(const char *qs, const char *key, char *out, size_t n);

/* xplane.c */
bool xp_axis_in_use(const settings_t *s, int target);
void xp_wanted_overrides(const settings_t *s, bool want[OV_COUNT]);
void xp_send_dref(core_t *c, const char *path, float value);
void xp_send_cmnd(core_t *c, const char *cmd);
void xp_engage(core_t *c, double now);
void xp_disengage(core_t *c, double now);
void xp_release_tick(core_t *c, double now);
void xp_push(core_t *c, double now);

/* telemetry.c */
void telemetry_tick(core_t *c, double now);
void telemetry_unsubscribe(core_t *c);
void core_on_udp(core_t *c, const unsigned char *buf, size_t n, bool from_loopback, double now);
void sim_release(core_t *c, const char *why);
int  telemetry_rref_count(void);                         /* ids 1..count */
bool telemetry_rref_path(int id, char *out, size_t n);
double turb_at_altitude(const float *turb, const float *alt, int n, double elev_m);

/* core.c */
void core_init(core_t *c, const settings_t *s, const core_sinks_t *sinks);
void core_on_report(core_t *c, const unsigned char *buf, long len, double now);
void core_on_device(core_t *c, bool bluetooth, bool added);
void core_set_gate(core_t *c, bool bt, bool usb, bool xp, const char *xp_exe, double now);
bool core_gate_due(const core_t *c, double now, double last_gate);
void core_apply_cmd(core_t *c, const cmd_t *cmd, double now);
void core_tick(core_t *c, double now);
void core_snapshot(const core_t *c, dsxp_snap_t *out);
void core_mark_dirty(core_t *c, double now);
void core_flush_save(core_t *c);
void out_tick(core_t *c, double now);
void out_quiet(core_t *c);
void out_mark(core_t *c);
bool core_input_live(const core_t *c, double now);

/* api.c - the JSON API, independent of how it is transported */
typedef struct {
    void *ctx;
    void (*snapshot)(void *ctx, dsxp_snap_t *out);
    bool (*submit)(void *ctx, const cmd_t *cmd);                   /* false = queue full */
    void (*catalogue)(void *ctx, void (*fn)(void *arg, const xcmd_t *cmds, int n), void *arg);
    void (*app_info)(void *ctx, jbuf_t *j);                        /* writes the "app" object */
} api_env_t;
void api_handle(const api_env_t *env, const char *method, const char *path,
                const char *query, dsxp_response_t *out);

/* ------------------------------------------------------- I/O modules */

/* core_thread.c */
double now_sec(void);                       /* monotonic seconds */

/* fileio.c */
char *file_read_all(const char *path, size_t max, size_t *len);
bool  file_write_atomic(const char *path, const void *buf, size_t len, unsigned mode);
bool  mkdir_p(const char *path, unsigned mode);

/* log.c */
void log_init(const char *dir, int retention_days, bool echo_stderr);
void log_shutdown(void);
void dsxp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_report_line(const char *line);
bool log_name_is_ours(const char *name);

/* gate.c */
bool gate_xplane_running(char *exe_out, size_t n);

#endif /* DSXP_CORE_INTERNAL_H */
