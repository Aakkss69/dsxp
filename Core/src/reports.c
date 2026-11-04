/*
 * Report accounting.
 *
 * The live counter is a rolling 10-second window, not a lifetime total - an
 * ever-climbing number tells you nothing, whereas a windowed count doubles as a
 * link-health readout. Over Bluetooth the DualSense delivers report 0x31 at
 * roughly 62 Hz, so a healthy window lands near 620; a sagging number means the
 * Bluetooth link is struggling.
 *
 * Each window is flushed to logs/reports-YYYY-MM-DD.log before the counter is
 * purged, so the history survives even though the display does not.
 */
#include "dsxp.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define WINDOW_SEC 10.0

static double g_window_start = 0;
static bool   g_logdir_ready = false;

static bool ensure_logdir(char *out, size_t n) {
    snprintf(out, n, "%s/logs", g_dir);
    if (g_logdir_ready) return true;
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[dsxp] cannot create %s: %s\n", out, strerror(errno));
        return false;
    }
    g_logdir_ready = true;
    return true;
}

void reports_tick(void) {
    double now = now_sec();
    if (g_window_start == 0) { g_window_start = now; return; }
    double elapsed = now - g_window_start;
    if (elapsed < WINDOW_SEC) return;

    pthread_mutex_lock(&g_lock);
    long n = g_reports;
    g_reports = 0;                       /* purge the live counter */
    g_reports_total += n;
    long total = g_reports_total;
    bool connected = g_have_report;
    bool engaged = g_engaged;
    pthread_mutex_unlock(&g_lock);
    g_window_start = now;

    char dir[PATH_MAX];
    if (!ensure_logdir(dir, sizeof dir)) return;

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char path[PATH_MAX + 64], stamp[32];
    snprintf(path, sizeof path, "%s/reports-%04d-%02d-%02d.log",
             dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    FILE *f = fopen(path, "a");
    if (!f) { fprintf(stderr, "[dsxp] cannot append %s: %s\n", path, strerror(errno)); return; }
    fprintf(f, "%s  reports=%-5ld rate=%6.1f Hz  total=%-8ld "
               "link=%s gate=[bt=%d usb=%d xp=%d] %s\n",
            stamp, n, n / elapsed, total,
            connected ? "up" : "down",
            g_bt_present, g_usb_present, g_xp_running,
            engaged ? "engaged" : "idle");
    fclose(f);

    if (g_verbose)
        fprintf(stderr, "[dsxp] window: %ld reports (%.1f Hz), total %ld\n",
                n, n / elapsed, total);
}
