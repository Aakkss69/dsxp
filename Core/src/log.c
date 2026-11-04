/*
 * Logging: os_log (Console.app, subsystem io.github.aakkss69.DSXP) plus daily
 * files in the log directory:
 *   events-YYYY-MM-DD.log   what the bridge did (engage, binds, errors)
 *   reports-YYYY-MM-DD.log  input report rate windows
 * Files older than the retention period are deleted at start and at every date
 * rollover; a file stops growing at 5 MB. All file work happens on a private
 * serial queue so callers never block on disk.
 */
#include "core_internal.h"
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE_CAP (5L * 1024 * 1024)

static struct {
    bool on, echo;
    char dir[1024];
    int  retention_days;
    int  last_yday, last_year;
    dispatch_queue_t q;
    os_log_t ol;
} L;

typedef struct { const char *prefix; char *line; } log_job_t;

bool log_name_is_ours(const char *name) {
    const char *rest;
    if (!strncmp(name, "reports-", 8)) rest = name + 8;
    else if (!strncmp(name, "events-", 7)) rest = name + 7;
    else return false;
    /* YYYY-MM-DD.log */
    static const char pat[] = "dddd-dd-dd.log";
    if (strlen(rest) != sizeof pat - 1) return false;
    for (size_t i = 0; i < sizeof pat - 1; i++) {
        if (pat[i] == 'd') { if (rest[i] < '0' || rest[i] > '9') return false; }
        else if (rest[i] != pat[i]) return false;
    }
    return true;
}

static void sweep(void) {
    DIR *d = opendir(L.dir);
    if (!d) return;
    time_t cutoff = time(NULL) - (time_t)L.retention_days * 86400;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!log_name_is_ours(e->d_name)) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", L.dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && st.st_mtime < cutoff) unlink(p);
    }
    closedir(d);
}

static void sweep_job(void *arg) { (void)arg; sweep(); }

static void append_job(void *arg) {
    log_job_t *j = arg;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_yday != L.last_yday || tm.tm_year != L.last_year) {
        L.last_yday = tm.tm_yday;
        L.last_year = tm.tm_year;
        sweep();
    }
    char path[1400], stamp[16];
    snprintf(path, sizeof path, "%s/%s-%04d-%02d-%02d.log", L.dir, j->prefix,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    struct stat st;
    if (!(stat(path, &st) == 0 && st.st_size > LOG_FILE_CAP)) {
        FILE *f = fopen(path, "a");
        if (f) { fprintf(f, "%s  %s\n", stamp, j->line); fclose(f); }
    }
    free(j->line);
    free(j);
}

static void enqueue(const char *prefix, const char *line) {
    if (!L.on || !L.q) return;
    log_job_t *j = malloc(sizeof *j);
    if (!j) return;
    j->prefix = prefix;
    j->line = strdup(line);
    if (!j->line) { free(j); return; }
    dispatch_async_f(L.q, j, append_job);
}

void log_init(const char *dir, int retention_days, bool echo_stderr) {
    snprintf(L.dir, sizeof L.dir, "%s", dir ? dir : "");
    L.retention_days = retention_days > 0 ? retention_days : 14;
    L.echo = echo_stderr;
    L.last_yday = -1;
    L.last_year = -1;
    if (!L.q) L.q = dispatch_queue_create("dsxp.log", DISPATCH_QUEUE_SERIAL);
    if (!L.ol) L.ol = os_log_create("io.github.aakkss69.DSXP", "core");
    L.on = L.dir[0] != 0;
    if (L.on) dispatch_async_f(L.q, NULL, sweep_job);
}

static void noop(void *arg) { (void)arg; }

void log_shutdown(void) {
    if (L.q) dispatch_sync_f(L.q, NULL, noop);   /* flush pending lines */
    L.on = false;
}

void dsxp_log(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (L.echo) fprintf(stderr, "[dsxp] %s\n", msg);
    if (!L.on) return;
    /* the SDK's os_log macro carries an attribute this clang warns about */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
    if (L.ol) os_log(L.ol, "%{public}s", msg);
#pragma clang diagnostic pop
    enqueue("events", msg);
}

void log_report_line(const char *line) {
    if (L.echo) fprintf(stderr, "[dsxp] %s\n", line);
    enqueue("reports", line);
}
