/*
 * DSXP core - public C API.
 *
 * This is the only header the Swift app sees. Everything behind it runs on a
 * dedicated core thread with its own run loop; every function here is safe to
 * call from any thread.
 */
#ifndef DSXP_CORE_H
#define DSXP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Events delivered to dsxp_config_t.on_event. Called on a core-owned thread:
 * hop to your UI thread before touching UI state. */
enum { DSXP_EV_STATUS = 1, DSXP_EV_COMMANDS_LOADED = 2 };
typedef void (*dsxp_event_fn)(void *ctx, int event);
/* Diagnostics: every raw Bluetooth input report, on the core thread. */
typedef void (*dsxp_raw_fn)(void *ctx, const unsigned char *report, long len);

typedef struct {
    const char *data_dir;          /* bridge.conf, controls.conf, feedback.conf     */
    const char *log_dir;           /* reports-*.log and events-*.log                */
    const char *commands_path;     /* X-Plane Resources/plugins/Commands.txt, or "" */
    const char *import_bindings;   /* legacy bindings.conf to import once, or NULL  */
    bool        verbose;           /* also echo log lines to stderr                 */
    bool        dry_run;           /* read the controller, send nothing to X-Plane  */
    dsxp_event_fn on_event;
    void       *event_ctx;
    dsxp_raw_fn on_raw_report;     /* optional, diagnostics only                    */
    void       *raw_ctx;
} dsxp_config_t;

typedef enum {
    DSXP_LINK_NONE = 0, DSXP_LINK_BT, DSXP_LINK_USB, DSXP_LINK_BT_AND_USB
} dsxp_link_t;

typedef enum {
    DSXP_ACCESS_GRANTED = 0, DSXP_ACCESS_DENIED, DSXP_ACCESS_UNKNOWN
} dsxp_access_t;

typedef struct {
    uint64_t seq;             /* increments on every published snapshot          */
    int      link;            /* dsxp_link_t                                     */
    bool     input_live;      /* Bluetooth reports arrived in the last 2 s       */
    bool     xp_running;      /* X-Plane 12 process found                        */
    bool     xp_answering;    /* RREF telemetry is arriving                      */
    bool     engaged;         /* flight controls active: DSXP flies X-Plane       */
    bool     suspended;       /* paused for system sleep                         */
    bool     imported_legacy; /* settings were imported from the old prototype   */
    int      hid_open_result; /* IOReturn of IOHIDManagerOpen, 0 = success       */
    int      ncmds;           /* X-Plane commands in the catalog               */
    double   report_hz;       /* input report rate over the last 10 s window     */
    int      xp_port;         /* X-Plane's UDP port (bridge.conf xplane.port)     */
    int      config_warnings; /* problems found in the config files              */
    char     config_warn_file[32];   /* first file with a problem, or ""         */
    char     xp_exe_path[1024];
    char     commands_path[1024];
} dsxp_status_t;

/* Start / stop. dsxp_stop releases X-Plane's controls, quiets the controller
 * and flushes settings before it returns (or after timeout_sec at most). */
int  dsxp_start(const dsxp_config_t *cfg);   /* 0 on success */
/* Returns false if the core did not finish within timeout_sec (0.1-10 s): the
 * crash guard should then be left to clean up, and dsxp_start will refuse. */
bool dsxp_stop(double timeout_sec);          /* idempotent   */
bool dsxp_is_running(void);

void dsxp_get_status(dsxp_status_t *out);
void dsxp_set_commands_path(const char *path);   /* reloads asynchronously */
void dsxp_set_suspended(bool suspended);
dsxp_access_t dsxp_input_access(bool request);   /* Input Monitoring (ListenEvent) */

/* The mapper UI's JSON API (same routes and fields as the old HTTP server). */
typedef struct {
    int         status;        /* HTTP status code                   */
    const char *content_type;  /* static string, do not free         */
    char       *body;          /* malloc'd; free with dsxp_response_free */
    size_t      len;
} dsxp_response_t;
void dsxp_api(const char *method, const char *path, const char *query,
              dsxp_response_t *out);
void dsxp_response_free(dsxp_response_t *r);

/* Crash guard: a helper process that cleans up if the app dies without doing so. */
/* Spawn it after dsxp_start: it inherits the core's UDP socket so it can cancel
 * the telemetry subscriptions X-Plane is streaming to that socket. */
int  dsxp_guard_spawn(const char *exe_path);  /* returns the pipe fd, or -1       */
void dsxp_guard_release(int fd);              /* clean exit: guard leaves quietly */
void dsxp_guard_port(int fd, int port);       /* X-Plane's port changed           */
int  dsxp_guard_main(int pipe_fd, int sock_fd, int port);   /* body of the guard  */

#ifdef __cplusplus
}
#endif
#endif /* DSXP_CORE_H */
