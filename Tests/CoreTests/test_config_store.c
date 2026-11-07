/* The config files on disk: first run, the two imports, edits made outside
 * DSXP (applied, waited for, ignored when only touched), merges with the
 * mapper's changes, deletion, and the write path. Also the gate's X-Plane
 * version check, which needs a folder on disk too. File times are set
 * explicitly, so nothing here depends on how fast the test runs. */
#include "tests.h"
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char ROOT[1024];
static cfg_store_t S;
static config_t L, D;
static char BUF[64 * 1024];
static int WRITES, SYNC_WRITES;

static int rm_entry(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(p);
}

static void make_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", ROOT, name);
    mkdir(out, 0700);
}

static void put_file(const char *dir, const char *name, const char *text, time_t mtime) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { CHECK(!"fopen"); return; }
    fputs(text, f);
    fclose(f);
    struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };
    utimensat(AT_FDCWD, p, ts, 0);
}

static void set_mtime(const char *dir, const char *name, time_t mtime) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };
    utimensat(AT_FDCWD, p, ts, 0);
}

static char *get_file(const char *dir, const char *name) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    size_t len = 0;
    return file_read_all(p, CONF_MAX_BYTES, &len);
}

static bool exists(const char *dir, const char *name) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static bool counting_write(void *ctx, const char *path, const char *text, size_t len, bool sync) {
    (void)ctx;
    WRITES++;
    if (sync) SYNC_WRITES++;
    return file_write_atomic(path, text, len, 0644);
}

static void first_run(time_t t0) {
    char d[1100];
    make_dir(d, sizeof d, "first");
    WRITES = SYNC_WRITES = 0;
    CHECK_INT(cfg_store_open(&S, d, NULL, &L, (double)t0, counting_write, NULL), 0);
    CHECK_INT(WRITES, 3); CHECK_INT(SYNC_WRITES, 3);
    CHECK(!S.imported_prototype && !S.migrated_v2);
    CHECK_INT(S.gen, 1);
    config_defaults(&D);
    for (int f = 0; f < CF_COUNT; f++) {
        const char *name = conf_schema((conf_file_t)f)->file;
        char *text = get_file(d, name);
        size_t n = conf_format((conf_file_t)f, &D, BUF, sizeof BUF);
        CHECK(text && strlen(text) == n && !memcmp(text, BUF, n));
        free(text);
        CHECK_INT(S.warns[f].n, 0);
        char p[1200];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", d, name);
        CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0644);
    }
    CHECK_NEAR(L.s.tune.deadzone, D.s.tune.deadzone, 0);
    CHECK_INT(L.bridge.port, 49000);
    /* nothing changed: a poll does nothing, and a flush writes nothing */
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t0 + 100), 0);
    CHECK_INT(cfg_store_flush(&S, &L, CFB_ALL, (double)t0 + 100, false, NULL), 0);
    CHECK_INT(WRITES, 3);
}

static void outside_edits(time_t t0) {
    char d[1100];
    make_dir(d, sizeof d, "edits");
    CHECK_INT(cfg_store_open(&S, d, NULL, &L, (double)t0, NULL, NULL), 0);

    /* an edit younger than a second is left for the next poll */
    time_t t = t0 + 10;
    put_file(d, "controls.conf", "stick.deadzone = 20 %\n", t);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 0.5), 0);
    CHECK_NEAR(L.s.tune.deadzone, 0.06, 1e-12);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 2), CFB_CONTROLS);
    CHECK_NEAR(L.s.tune.deadzone, 0.20, 1e-12);
    CHECK_NEAR(L.s.tune.curve, 1.0, 1e-12);          /* keys not in the file: defaults */
    CHECK_INT(S.gen, 2);
    CHECK_INT(S.last_reload, CF_CONTROLS + 1);

    /* touched (new time, same bytes): ignored */
    set_mtime(d, "controls.conf", t + 5);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 10), 0);
    CHECK_INT(S.gen, 2);

    /* a bad line: everything else applies, the line is reported with its number */
    put_file(d, "controls.conf", "stick.deadzone = 20 %\nstick.curve = banana\nstick.deadzone = 15 %\n", t + 20);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 30), CFB_CONTROLS);
    CHECK_NEAR(L.s.tune.deadzone, 0.15, 1e-12);
    CHECK_NEAR(L.s.tune.curve, 1.0, 1e-12);
    CHECK_INT(S.warns[CF_CONTROLS].n, 2);
    CHECK_INT(S.warns[CF_CONTROLS].w[0].line, 2);
    CHECK(!strcmp(S.warns[CF_CONTROLS].w[0].key, "stick.curve"));
    /* fixing it clears the warnings */
    put_file(d, "controls.conf", "stick.deadzone = 15 %\n", t + 40);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 50), CFB_CONTROLS);
    CHECK_INT(S.warns[CF_CONTROLS].n, 0);

    /* a cross-key rule broken in the file: corrected, and reported */
    put_file(d, "feedback.conf", "rumble.g_jolt_start = 5.0 g\nrumble.g_jolt_full = 3.0 g\n", t + 60);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 70), CFB_FEEDBACK);
    CHECK_NEAR(L.s.rumble.jolt_full_g, 5.5, 1e-12);
    CHECK_INT(S.warns[CF_FEEDBACK].n, 1);

    /* bridge.conf is read the same way */
    put_file(d, "bridge.conf", "telemetry.rate = 20 Hz\nxplane.port = 49005\n", t + 80);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 90), CFB_BRIDGE);
    CHECK_NEAR(L.bridge.telemetry_hz, 20, 1e-12);
    CHECK_INT(L.bridge.port, 49005);

    /* the mapper changed the curve; the file's deadzone was edited meanwhile:
     * both survive, on the poll and in the file the flush writes */
    L.s.tune.curve = 2.0;
    put_file(d, "controls.conf", "stick.deadzone = 25 %\n", t + 100);
    CHECK_INT(cfg_store_poll(&S, &L, CFB_CONTROLS, (double)t + 110), CFB_CONTROLS);
    CHECK_NEAR(L.s.tune.curve, 2.0, 1e-12);
    CHECK_NEAR(L.s.tune.deadzone, 0.25, 1e-12);
    CHECK_INT(cfg_store_flush(&S, &L, CFB_CONTROLS, (double)t + 111, true, NULL), 0);
    char *text = get_file(d, "controls.conf");
    CHECK(text && strstr(text, "stick.curve = 2.00\n") && strstr(text, "stick.deadzone = 25.0 %\n"));
    CHECK(text && strstr(text, "# Response curve") != NULL);   /* written with its comments */
    free(text);
    /* our own write is not taken for an outside edit */
    unsigned gen = S.gen;
    CHECK_INT(cfg_store_poll(&S, &L, 0, wall_now() + 5), 0);
    CHECK_INT(S.gen, gen);

    /* an outside edit not polled yet when the mapper saves: merged at the flush */
    text = get_file(d, "controls.conf");
    char *at = text ? strstr(text, "stick.deadzone = 25.0 %") : NULL;
    CHECK(at != NULL);
    if (at) { at[17] = '3'; at[18] = '0'; }           /* 25.0 -> 30.0 */
    put_file(d, "controls.conf", text ? text : "", t + 200);
    free(text);
    L.s.tune.gesture = 0.8;
    S.last_reload = 0;
    CHECK_INT(cfg_store_flush(&S, &L, CFB_CONTROLS, (double)t + 210, false, NULL), CFB_CONTROLS);
    CHECK_INT(S.last_reload, CF_CONTROLS + 1);       /* the mapper shows "applied your edit" */
    CHECK_NEAR(L.s.tune.deadzone, 0.30, 1e-12);
    CHECK_NEAR(L.s.tune.gesture, 0.8, 1e-12);
    CHECK_NEAR(L.s.tune.curve, 2.0, 1e-12);
    text = get_file(d, "controls.conf");
    CHECK(text && strstr(text, "stick.deadzone = 30.0 %\n") && strstr(text, "gesture.sensitivity = 80.0 %\n"));
    free(text);

    /* bridge.conf is never rewritten by a save */
    text = get_file(d, "bridge.conf");
    L.bridge.port = 49999;
    cfg_store_flush(&S, &L, CFB_ALL, (double)t + 220, true, NULL);
    char *after = get_file(d, "bridge.conf");
    CHECK(text && after && !strcmp(text, after));
    free(text); free(after);
    L.bridge.port = 49005;

    /* deleted: written back with its defaults after two polls */
    char p[1200];
    snprintf(p, sizeof p, "%s/feedback.conf", d);
    L.s.light.on = false;
    unlink(p);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 300), 0);
    CHECK(!exists(d, "feedback.conf"));
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 302), CFB_FEEDBACK);
    CHECK(exists(d, "feedback.conf"));
    CHECK(L.s.light.on);
    CHECK_NEAR(L.s.rumble.jolt_full_g, RUMBLE_DEFAULTS.jolt_full_g, 1e-12);
    CHECK_INT(S.warns[CF_FEEDBACK].n, 0);
    CHECK_NEAR(L.s.tune.deadzone, 0.30, 1e-12);       /* other files untouched */
}

static const char V2[] =
    "tune deadzone 0.1234\ntune curve 2.5000\ntune gesture 0.7500\n"
    "light 1 2 3 1 0 0.500 4 1.000 0\nfxmode manual\nrumblemode manual\nrumblepath classic\n"
    "button 2 0 sim/flight_controls/landing_gear_down\n";

static void imports(time_t t0) {
    /* bindings.conf from the previous DSXP build: imported, then set aside */
    char d[1100];
    make_dir(d, sizeof d, "v2");
    put_file(d, "bindings.conf", V2, t0);
    CHECK_INT(cfg_store_open(&S, d, NULL, &L, (double)t0, NULL, NULL), 0);
    CHECK(S.migrated_v2); CHECK(!S.imported_prototype);
    CHECK(!exists(d, "bindings.conf"));
    CHECK(exists(d, "bindings.conf.imported"));
    CHECK_NEAR(L.s.tune.deadzone, 0.1234, 1e-12);
    char *text = get_file(d, "controls.conf");
    CHECK(text && strstr(text, "stick.deadzone = 12.3 %\n") && strstr(text, "stick.curve = 2.50\n"));
    free(text);
    text = get_file(d, "feedback.conf");
    CHECK(text && strstr(text, "light.color = #010203\n"));
    free(text);
    /* the next start reads the files and imports nothing */
    CHECK_INT(cfg_store_open(&S, d, NULL, &L, (double)t0, NULL, NULL), 0);
    CHECK(!S.migrated_v2);
    CHECK_NEAR(L.s.tune.deadzone, 0.123, 1e-12);      /* what the file says, at its precision */

    /* the prototype's file: imported once, never changed */
    char d2[1100], proto[1200];
    make_dir(d2, sizeof d2, "proto");
    snprintf(proto, sizeof proto, "%s/proto-bindings.conf", ROOT);
    put_file(ROOT, "proto-bindings.conf", V2, t0);
    CHECK_INT(cfg_store_open(&S, d2, proto, &L, (double)t0, NULL, NULL), 0);
    CHECK(S.imported_prototype); CHECK(!S.migrated_v2);
    CHECK(exists(ROOT, "proto-bindings.conf"));
    CHECK(!strcmp(L.s.bind[B_DP_R].cmd, "sim/flight_controls/landing_gear_down"));
    CHECK_INT(cfg_store_open(&S, d2, proto, &L, (double)t0, NULL, NULL), 0);
    CHECK(!S.imported_prototype);                     /* once only */

    /* config files already there: an old bindings.conf is left alone */
    char d3[1100];
    make_dir(d3, sizeof d3, "both");
    put_file(d3, "bindings.conf", V2, t0);
    put_file(d3, "controls.conf", "stick.deadzone = 9 %\n", t0);
    CHECK_INT(cfg_store_open(&S, d3, NULL, &L, (double)t0, NULL, NULL), 0);
    CHECK(!S.migrated_v2);
    CHECK(exists(d3, "bindings.conf"));
    CHECK_NEAR(L.s.tune.deadzone, 0.09, 1e-12);
    text = get_file(d3, "controls.conf");
    CHECK(text && !strcmp(text, "stick.deadzone = 9 %\n"));   /* the user's file is not rewritten */
    free(text);

    /* a folder path that would be truncated is refused */
    char longp[1100];
    memset(longp, 'a', sizeof longp - 1);
    longp[0] = '/';
    longp[sizeof longp - 1] = 0;
    CHECK_INT(cfg_store_open(&S, longp, NULL, &L, (double)t0, NULL, NULL), -1);
}

/* F9: any install folder, but only X-Plane 12 or later (read from Info.plist) */
static void gate_version(void) {
    const char *vers[] = { "12.4.4-b3-5bfd5a53", "11.55r2", "13.0.0", "garbage" };
    bool want[] = { true, false, true, false };
    for (int i = 0; i < 4; i++) {
        char app[1200], exe[1300], plist[1300];
        snprintf(app, sizeof app, "%s/sims %d/My Sim/X-Plane.app", ROOT, i);
        snprintf(exe, sizeof exe, "%s/Contents/MacOS", app);
        CHECK(mkdir_p(exe, 0700));
        snprintf(exe, sizeof exe, "%s/Contents/MacOS/X-Plane", app);
        snprintf(plist, sizeof plist, "%s/Contents/Info.plist", app);
        char text[600];
        snprintf(text, sizeof text,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                 "<key>CFBundleShortVersionString</key><string>%s</string></dict></plist>\n", vers[i]);
        CHECK(file_write_atomic(plist, text, strlen(text), 0644));
        CHECK_INT(gate_version_ok(exe), want[i]);
    }
    CHECK(!gate_version_ok("/Applications/X-Plane 12/X-Plane.app/Contents/MacOS/X-Plane Installer"));
    CHECK(!gate_version_ok("/nowhere/X-Plane.app/Contents/MacOS/X-Plane"));     /* no Info.plist */
}

/* Every applied edit is checked as a whole, at the poll and at a save; an edit still
 * landing makes a save wait; a rule across files is applied; the pulse fix is written so
 * it passes; an unreadable file is reported; a deleted folder is made again. */
static void store_checks(time_t t0) {
    char d[1100];
    make_dir(d, sizeof d, "checks");
    CHECK_INT(cfg_store_open(&S, d, NULL, &L, (double)t0, NULL, NULL), 0);
    time_t t = t0 + 10;

    /* merged at a save: the merged result is checked (it used to be written back with
     * a swipe shorter than the tap wander) */
    put_file(d, "controls.conf", "gesture.swipe_length_at_100 = 3.0 %\n", t);
    L.s.tune.curve = 2.0;                                       /* the mapper's change */
    unsigned later = 99;
    CHECK(cfg_store_flush(&S, &L, CFB_CONTROLS, (double)t + 5, false, &later) & CFB_CONTROLS);
    CHECK_INT(later, 0);
    CHECK(L.s.gesture.swipe_length_at100 > L.s.gesture.tap_wander_at100);
    CHECK(S.warns[CF_CONTROLS].n >= 1);
    CHECK_NEAR(L.s.tune.curve, 2.0, 1e-12);

    /* an edit younger than a second when the mapper saves: the save waits for it */
    put_file(d, "controls.conf", "stick.deadzone = 12 %\n", t + 20);
    CHECK_INT(cfg_store_flush(&S, &L, CFB_CONTROLS, (double)t + 20.5, false, &later), 0);
    CHECK_INT(later, CFB_CONTROLS);
    char *text = get_file(d, "controls.conf");                 /* not overwritten         */
    CHECK(text && !strcmp(text, "stick.deadzone = 12 %\n"));
    free(text);
    CHECK(cfg_store_flush(&S, &L, CFB_CONTROLS, (double)t + 22, false, &later) & CFB_CONTROLS);
    CHECK_NEAR(L.s.tune.deadzone, 0.12, 1e-12);

    /* a rule across files: a lower tick rate in bridge.conf raises gesture.pulse, with a
     * warning on controls.conf (it used to stay below 1.5 ticks) */
    L.s.gesture.pulse_s = 0.025;
    put_file(d, "bridge.conf", "timing.tick_rate = 30 Hz\n", t + 40);
    unsigned ch = cfg_store_poll(&S, &L, 0, (double)t + 45);
    CHECK((ch & CFB_BRIDGE) && (ch & CFB_CONTROLS));
    CHECK_NEAR(L.s.gesture.pulse_s, 0.05, 1e-12);
    CHECK(S.warns[CF_CONTROLS].n >= 1);

    /* the pulse fix is whole milliseconds that pass the check: 70 Hz needs 21.43 ms,
     * written as 22 (21 failed the check again on every reload) */
    static config_t pc;
    static conf_warns_t pw[CF_COUNT];
    config_defaults(&pc);
    pc.bridge.tick_hz = 70;
    pc.s.gesture.pulse_s = 0.010;
    memset(pw, 0, sizeof pw);
    config_validate(&pc, pw);
    CHECK_NEAR(pc.s.gesture.pulse_s, 0.022, 1e-12);
    memset(pw, 0, sizeof pw);
    config_validate(&pc, pw);
    CHECK_INT(pw[CF_CONTROLS].n, 0);

    /* a file that cannot be read is reported (the warning used to be dropped) */
    char p[1200];
    snprintf(p, sizeof p, "%s/feedback.conf", d);
    put_file(d, "feedback.conf", "light.on = no\n", t + 60);
    chmod(p, 0);
    unsigned gen = S.gen;
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 65), 0);
    CHECK(S.gen != gen && S.warns[CF_FEEDBACK].n == 1);
    chmod(p, 0644);

    /* the whole folder deleted: made again and the defaults written back, once (the
     * settings used to flip to defaults and back every two polls) */
    nftw(d, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    L.s.tune.deadzone = 0.30;
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 80), 0);   /* gone for one poll       */
    CHECK(cfg_store_poll(&S, &L, 0, (double)t + 82) & CFB_CONTROLS);
    CHECK(exists(d, "controls.conf"));
    CHECK_NEAR(L.s.tune.deadzone, 0.06, 1e-12);
    L.s.tune.deadzone = 0.30;                                   /* the mapper sets it again */
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 84), 0);
    CHECK_NEAR(L.s.tune.deadzone, 0.30, 1e-12);
    /* a deleted file that cannot be written back: the settings in use stay */
    snprintf(p, sizeof p, "%s/controls.conf", d);
    unlink(p);
    chmod(d, 0500);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 90), 0);
    CHECK_INT(cfg_store_poll(&S, &L, 0, (double)t + 92) & CFB_CONTROLS, 0);
    CHECK(S.unwritable & CFB_CONTROLS);
    CHECK_NEAR(L.s.tune.deadzone, 0.30, 1e-12);
    chmod(d, 0700);
}

void test_config_store(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(ROOT, sizeof ROOT, "%s/dsxp-conf-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(ROOT)) { CHECK(!"mkdtemp"); return; }
    time_t t0 = (time_t)wall_now();
    first_run(t0);
    outside_edits(t0 + 1000);
    store_checks(t0 + 2000);
    imports(t0);
    gate_version();
    nftw(ROOT, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}
