/*
 * The config files on disk, kept in step with the running core (core thread).
 *
 *   open   first run writes the three files; settings from an older DSXP
 *          (bindings.conf) or the prototype are imported once
 *   poll   every 2 s: a file someone edited is parsed and applied, and the whole
 *          configuration is checked again (accept_edit); a file younger than 1 s
 *          is left for the next poll (an editor may still be writing it); a file
 *          only touched (same bytes) is ignored; a file missing for two polls in a
 *          row is written back with its defaults (its folder made again if gone)
 *   flush  saves what the mapper changed. If the file was edited meanwhile,
 *          keys the mapper changed win and every other key keeps the file's
 *          value, so neither side's change is lost; an edit still landing makes
 *          the save wait.
 */
#include "core_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

uint64_t conf_hash(const char *text, size_t len) {          /* FNV-1a, 64 bit */
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)text[i]; h *= 1099511628211ULL; }
    return h;
}

static file_sig_t sig_of(const char *path) {
    file_sig_t s;
    memset(&s, 0, sizeof s);
    struct stat st;
    if (stat(path, &st) == 0) {
        s.exists = true;
        s.mtime = st.st_mtimespec;
        s.size = st.st_size;
        s.ino = st.st_ino;
    }
    return s;
}

static bool sig_eq(const file_sig_t *a, const file_sig_t *b) {
    return a->exists == b->exists && a->size == b->size && a->ino == b->ino &&
           a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec;
}

static bool write_file(cfg_store_t *st, conf_file_t f, const config_t *c, bool sync) {
    static char buf[64 * 1024];
    size_t n = conf_format(f, c, buf, sizeof buf);
    if (n == 0) return false;
    uint64_t h = conf_hash(buf, n);
    if (h == st->hash[f] && sig_of(st->path[f]).exists) return true;   /* nothing changed */
    bool okw = st->write ? st->write(st->write_ctx, st->path[f], buf, n, sync)
                         : file_write_atomic(st->path[f], buf, n, 0644);
    if (okw) st->hash[f] = h;
    return okw;
}

/* Read and parse a file into *theirs (starting from the base values). Returns
 * false if it cannot be read; *same is set when the bytes match the last hash. */
static bool read_parse(cfg_store_t *st, conf_file_t f, config_t *theirs, conf_warns_t *w,
                       uint64_t *hash, bool *same) {
    size_t len = 0;
    char *text = file_read_all(st->path[f], CONF_MAX_BYTES, &len);
    if (!text) {
        if (sig_of(st->path[f]).exists) conf_warn(w, 0, "", "cannot read the file (over %d KB?)", CONF_MAX_BYTES / 1024);
        return false;
    }
    *hash = conf_hash(text, len);
    *same = *hash == st->hash[f];
    if (!*same) conf_parse(f, text, len, theirs, w);
    free(text);
    return true;
}

int cfg_store_open(cfg_store_t *st, const char *dir, const char *prototype, config_t *out,
                   double wall, conf_write_fn write, void *ctx) {
    (void)wall;
    memset(st, 0, sizeof *st);
    st->write = write;
    st->write_ctx = ctx;
    if (snprintf(st->dir, sizeof st->dir, "%s", dir) >= (int)sizeof st->dir) return -1;
    for (int f = 0; f < CF_COUNT; f++)
        if (snprintf(st->path[f], sizeof st->path[f], "%s/%s", dir, conf_schema((conf_file_t)f)->file)
            >= (int)sizeof st->path[f]) return -1;
    mkdir_p(dir, 0700);
    config_defaults(out);

    bool have[CF_COUNT];
    for (int f = 0; f < CF_COUNT; f++) have[f] = sig_of(st->path[f]).exists;

    /* settings from before the config files: import once */
    if (!have[CF_CONTROLS] && !have[CF_FEEDBACK]) {
        char v2[1200];
        snprintf(v2, sizeof v2, "%s/bindings.conf", dir);
        size_t len = 0;
        char *text = file_read_all(v2, SETTINGS_MAX_BYTES, &len);
        if (text) {
            bindings_parse(&out->s, text, len);
            free(text);
            char done[1300];
            snprintf(done, sizeof done, "%s.imported", v2);
            rename(v2, done);
            st->migrated_v2 = true;
            dsxp_log("imported settings from %s (kept as bindings.conf.imported)", v2);
        } else if (prototype && *prototype && (text = file_read_all(prototype, SETTINGS_MAX_BYTES, &len))) {
            bindings_parse(&out->s, text, len);
            free(text);
            st->imported_prototype = true;
            dsxp_log("imported settings from %s", prototype);
        }
    }

    for (int f = 0; f < CF_COUNT; f++) {
        if (!have[f]) continue;
        uint64_t h; bool same;
        st->hash[f] = 0;
        if (read_parse(st, (conf_file_t)f, out, &st->warns[f], &h, &same)) st->hash[f] = h;
    }
    config_validate(out, st->warns);
    for (int f = 0; f < CF_COUNT; f++) {
        for (int i = 0; i < st->warns[f].n; i++)
            dsxp_log("config: %s line %d %s: %s", conf_schema((conf_file_t)f)->file,
                     st->warns[f].w[i].line, st->warns[f].w[i].key, st->warns[f].w[i].msg);
        if (!have[f]) {
            write_file(st, (conf_file_t)f, out, true);
            dsxp_log("wrote %s", st->path[f]);
        }
        st->sig[f] = sig_of(st->path[f]);
    }
    st->base = *out;
    st->gen = 1;
    return 0;
}

/* An edit from outside DSXP has just been applied to `live` (file f; its parse warnings
 * in `parsed`). A rule can span keys the edit did not touch, or another file
 * (gesture.pulse against bridge.conf's tick rate), so the whole configuration is checked
 * again and each correction is kept as a warning on the file it belongs to. Returns the
 * files whose values in `live` changed: f, and any other file a rule corrected. */
static unsigned accept_edit(cfg_store_t *st, conf_file_t f, config_t *live,
                            const conf_warns_t *parsed, const char *how) {
    static conf_warns_t vw[CF_COUNT];               /* core thread only; too big for the stack */
    memset(vw, 0, sizeof vw);
    config_validate(live, vw);                      /* the merged result, every rule   */
    st->warns[f] = *parsed;
    unsigned changed = 1u << f;
    for (int g = 0; g < CF_COUNT; g++) {
        for (int i = 0; i < vw[g].n; i++) conf_warn(&st->warns[g], 0, vw[g].w[i].key, "%s", vw[g].w[i].msg);
        if (vw[g].n) changed |= 1u << g;            /* a rule changed this file's values */
    }
    st->gen++;
    st->last_reload = (int)f + 1;
    for (int i = 0; i < parsed->n; i++)
        dsxp_log("config: %s line %d %s: %s", conf_schema(f)->file, parsed->w[i].line,
                 parsed->w[i].key, parsed->w[i].msg);
    for (int g = 0; g < CF_COUNT; g++)
        for (int i = 0; i < vw[g].n; i++)
            dsxp_log("config: %s %s: %s", conf_schema((conf_file_t)g)->file, vw[g].w[i].key, vw[g].w[i].msg);
    dsxp_log("applied your edit to %s%s", conf_schema(f)->file, how);
    return changed;
}

/* Seconds since the file was last written (by anyone), from its signature. */
static double age_of(const file_sig_t *s, double wall) {
    return wall - ((double)s->mtime.tv_sec + s->mtime.tv_nsec / 1e9);
}

unsigned cfg_store_poll(cfg_store_t *st, config_t *live, unsigned dirty, double wall) {
    unsigned changed = 0;
    static config_t theirs, def;
    for (int fi = 0; fi < CF_COUNT; fi++) {
        conf_file_t f = (conf_file_t)fi;
        file_sig_t s = sig_of(st->path[f]);
        if (!s.exists) {
            if (++st->missing_polls[f] < 2) continue;   /* gone for one poll: may be a save */
            st->missing_polls[f] = 0;
            /* deleted: written back with its defaults, and only then are the defaults
             * used. The whole folder may be gone too, so it is made again first; a file
             * that still cannot be written keeps the settings in use (said once). */
            config_defaults(&def);
            mkdir_p(st->dir, 0700);
            st->hash[f] = 0;
            if (write_file(st, f, &def, true)) {
                config_copy_file(live, &def, f);
                config_copy_file(&st->base, &def, f);
                st->sig[f] = sig_of(st->path[f]);
                memset(&st->warns[f], 0, sizeof st->warns[f]);
                st->unwritable &= ~(1u << f);
                dsxp_log("%s was deleted: wrote it back with defaults", conf_schema(f)->file);
                changed |= 1u << f;
            } else if (!(st->unwritable & (1u << f))) {
                st->unwritable |= 1u << f;
                dsxp_log("%s was deleted and cannot be written back; the settings in use stay",
                         conf_schema(f)->file);
            }
            continue;
        }
        st->missing_polls[f] = 0;
        if (sig_eq(&s, &st->sig[f])) continue;
        if (age_of(&s, wall) < CONF_FRESH_SEC) continue;   /* may still be mid-save */

        theirs = st->base;
        conf_warns_t w;
        memset(&w, 0, sizeof w);
        uint64_t h; bool same;
        bool read = read_parse(st, f, &theirs, &w, &h, &same);
        st->sig[f] = s;
        if (!read) {                                        /* cannot be read: say so  */
            if (w.n) {
                st->warns[f] = w;
                st->gen++;
                dsxp_log("config: %s: %s", conf_schema(f)->file, w.w[0].msg);
            }
            continue;
        }
        if (same) continue;                                 /* touched, not edited */
        if (dirty & (1u << f)) conf_merge(f, &st->base, live, &theirs, live);
        else config_copy_file(live, &theirs, f);
        config_copy_file(&st->base, &theirs, f);
        st->hash[f] = h;
        changed |= accept_edit(st, f, live, &w, "");
    }
    return changed;
}

unsigned cfg_store_flush(cfg_store_t *st, config_t *live, unsigned files, double wall, bool sync,
                         unsigned *later) {
    unsigned changed = 0;
    if (later) *later = 0;
    static config_t theirs;
    for (int fi = 0; fi < CF_COUNT; fi++) {
        conf_file_t f = (conf_file_t)fi;
        if (!(files & (1u << f)) || !conf_schema(f)->rewritten) continue;
        file_sig_t s = sig_of(st->path[f]);
        if (s.exists && !sig_eq(&s, &st->sig[f])) {
            /* edited outside DSXP since we last looked: keep both sides' changes. An edit
             * younger than CONF_FRESH_SEC may still be being written: the save waits for
             * it (*later) rather than merging half a file, except at quit (sync). */
            if (!sync && later && age_of(&s, wall) < CONF_FRESH_SEC) {
                *later |= 1u << f;
                continue;
            }
            theirs = st->base;
            conf_warns_t w;
            memset(&w, 0, sizeof w);
            uint64_t h; bool same;
            if (read_parse(st, f, &theirs, &w, &h, &same) && !same) {
                conf_merge(f, &st->base, live, &theirs, live);
                changed |= accept_edit(st, f, live, &w, " (merged with the mapper's changes)");
            }
        }
        if (write_file(st, f, live, sync)) {
            config_copy_file(&st->base, live, f);
            /* the next poll sees our own write: same bytes, so only the signature is updated */
        }
    }
    return changed;
}
