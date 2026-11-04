/*
 * The three user config files: schema, parser, writer, and the store that keeps
 * them in sync with the running core. The .conf files in configs/ are its output.
 */
#ifndef DSXP_CONFIG_H
#define DSXP_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

typedef enum { CF_BRIDGE = 0, CF_CONTROLS, CF_FEEDBACK, CF_COUNT } conf_file_t;
enum { CFB_BRIDGE = 1, CFB_CONTROLS = 2, CFB_FEEDBACK = 4, CFB_ALL = 7 };

/* Everything the three files hold. */
typedef struct { bridge_cfg_t bridge; settings_t s; } config_t;

typedef enum {
    CT_SECTION,   /* not a key: a section banner                         */
    CT_INT,       /* int                                                 */
    CT_U8,        /* unsigned char, written as an integer                */
    CT_REAL,      /* double                                              */
    CT_PCT,       /* double 0..1, written as percent                     */
    CT_PCTB,      /* unsigned char 0..255, written as percent            */
    CT_TIME,      /* double seconds, written in the key's unit (s or ms) */
    CT_BOOL,      /* bool, yes/no                                        */
    CT_ENUM,      /* int, one of names[]                                 */
    CT_ENUMB,     /* bool, names[0] = false, names[1] = true             */
    CT_COLOR,     /* 3 bytes r g b, #RRGGBB                              */
    CT_LAMPS,     /* unsigned char 5-bit lamp mask                       */
    CT_BUTTON,    /* binding_t: command [hold|once] or none              */
    CT_AXIS,      /* axis_map_t: target [inverted]                       */
} conf_type_t;

typedef struct {
    const char *key;                 /* CT_SECTION: the section title          */
    conf_type_t type;
    size_t      off;                 /* offsetof(config_t, ...)                */
    double      lo, hi;              /* range in the written unit              */
    int         decimals;
    const char *unit;                /* "%", "s", "ms", "Hz", "kt", "g", ...   */
    const char *const *names;        /* CT_ENUM / CT_ENUMB                     */
    int         nnames;
    const char *doc;                 /* comment text, lines separated by '\n'  */
} conf_key_t;

typedef struct {
    const char *file;                /* "controls.conf"                        */
    const char *title;               /* "controls"                             */
    const char *intro;
    bool        rewritten;           /* the mapper writes this file            */
    const conf_key_t *keys;
    int         nkeys;
} conf_schema_t;

#define CONF_WARN_MAX 32
#define CONF_MAX_BYTES (256 * 1024)
#define CONFIG_POLL_SEC 2.0          /* fixed: a bad edit cannot slow its own fix */
#define CONF_FRESH_SEC 1.0           /* a file this fresh may still be mid-save   */
typedef struct { int line; char key[48]; char msg[160]; } conf_warn_t;
typedef struct { conf_warn_t w[CONF_WARN_MAX]; int n, dropped; } conf_warns_t;

/* settings/config_schema.c */
const conf_schema_t *conf_schema(conf_file_t f);

/* settings/settings.c */
void config_defaults(config_t *c);

/* settings/conf.c */
bool   conf_parse(conf_file_t f, const char *text, size_t len, config_t *io, conf_warns_t *w);
size_t conf_format(conf_file_t f, const config_t *c, char *buf, size_t cap);
size_t conf_format_value(const conf_key_t *k, const config_t *c, char *out, size_t n);
void   conf_copy_key(config_t *dst, const config_t *src, const conf_key_t *k);
void   config_copy_file(config_t *dst, const config_t *src, conf_file_t f);
void   conf_merge(conf_file_t f, const config_t *base, const config_t *ours,
                  const config_t *theirs, config_t *out);
void   config_validate(config_t *c, conf_warns_t w[CF_COUNT]);
void   conf_warn(conf_warns_t *w, int line, const char *key, const char *fmt, ...)
           __attribute__((format(printf, 4, 5)));
const conf_key_t *conf_find(conf_file_t f, const char *key);
int    config_write_default_files(const char *dir);

/* runtime/config_store.c - the files on disk (core thread only) */
typedef bool (*conf_write_fn)(void *ctx, const char *path, const char *text, size_t len, bool sync);
typedef struct { struct timespec mtime; off_t size; ino_t ino; bool exists; } file_sig_t;
typedef struct {
    char       dir[1024], path[CF_COUNT][1100];
    file_sig_t sig[CF_COUNT];
    uint64_t   hash[CF_COUNT];        /* FNV-1a of the text last read or written */
    int        missing_polls[CF_COUNT];
    config_t   base;                  /* file values as last read or written     */
    conf_warns_t warns[CF_COUNT];
    unsigned   gen;                   /* bumped whenever a file was applied      */
    int        last_reload;           /* file applied from an outside edit + 1   */
    bool       imported_prototype, migrated_v2;
    conf_write_fn write;
    void      *write_ctx;
} cfg_store_t;

uint64_t conf_hash(const char *text, size_t len);
int      cfg_store_open(cfg_store_t *st, const char *dir, const char *prototype,
                        config_t *out, double wall, conf_write_fn write, void *ctx);
unsigned cfg_store_poll(cfg_store_t *st, config_t *live, unsigned dirty, double wall);
unsigned cfg_store_flush(cfg_store_t *st, config_t *live, unsigned files, double wall, bool sync);
double   wall_now(void);

#endif /* DSXP_CONFIG_H */
