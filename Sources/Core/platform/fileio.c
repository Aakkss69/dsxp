/* Small file helpers: bounded reads, atomic writes, mkdir -p. */
#include "core_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *file_read_all(const char *path, size_t max, size_t *len) {
    *len = 0;
    /* O_NONBLOCK: a named pipe (FIFO) put where a file should be would block open()
     * forever; it is then turned away by the regular-file check below */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) {
        close(fd);
        return NULL;
    }
    size_t want = (size_t)st.st_size;
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, want - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    *len = got;
    return buf;
}

/* Write to a private temp file, fsync, then rename over the target: a crash
 * mid-write leaves the old file intact instead of a truncated one. */
bool file_write_atomic(const char *path, const void *buf, size_t len, unsigned mode) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) return false;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)mode);
    if (fd < 0) return false;
    const char *p = buf;
    size_t left = len;
    bool okw = true;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { okw = false; break; }
        p += w;
        left -= (size_t)w;
    }
    if (okw && fsync(fd) != 0) okw = false;
    if (close(fd) != 0) okw = false;
    if (!okw || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

bool mkdir_p(const char *path, unsigned mode) {
    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof buf, "%s", path);
    if (n <= 0 || (size_t)n >= sizeof buf) return false;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST) return false;
    return true;
}
