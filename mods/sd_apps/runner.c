#define _GNU_SOURCE
#include "package.h"
#include "sha256.h"
#include "natartos_app.h"
#include "natartos_display.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_NODES 512
#define MAX_DEPTH 8
#define PACKAGE_BYTES (64ULL * 1024 * 1024)
#define CACHE_BYTES (256ULL * 1024 * 1024)
#define LOG_BYTES (1024 * 1024)
#define CACHE_DEFAULT "/data/local/tmp/natartos-apps"
#define DATA_DEFAULT "/data/natartos/appdata"
/* The overlay's flag/frame files (hook_overlay.so, deploy/inject.sh) live here. */
#define DISPLAY_DEFAULT "/data/local/tmp"
#define MENU_LIST_SECONDS 5
#define MENU_LIST_MAX 128

static char error[512];
static volatile sig_atomic_t interrupted;
static int fail(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(error, sizeof(error), format, ap);
    va_end(ap);
    return -1;
}
static int syserror(const char *what) { return fail("%s: %s", what, strerror(errno)); }
/* O_NOFOLLOW reports a link as ELOOP; name other failures (e.g. EACCES) as they are. */
static int open_error(const char *what) {
    return errno == ELOOP ? fail("%s: links are not allowed", what) : syserror(what);
}
static void on_signal(int sig) { interrupted = sig; }

static int directory_access(int parent, const char *name, int create, int access) {
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
        return syserror("create directory");
    int fd = openat(parent, name, access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return open_error("open directory");
    return fd;
}
static int directory(int parent, const char *name, int create) {
    return directory_access(parent, name, create, O_RDONLY);
}

/* Walk absolute roots too: O_NOFOLLOW on just the final component is insufficient. */
static int root_directory(const char *path, int create, char resolved[PATH_MAX]) {
    if (*path != '/' || strlen(path) >= PATH_MAX) return fail("roots must be absolute paths");
    char copy[PATH_MAX];
    strcpy(copy, path);
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return syserror("open root");
    char *save, *part = strtok_r(copy, "/", &save);
    while (part) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(fd); return fail("dot components are not allowed in roots");
        }
        /* Ancestors require search access, not permission to list their contents. */
        int next = directory_access(fd, part, create, O_PATH);
        close(fd);
        if (next < 0) return -1;
        fd = next;
        part = strtok_r(NULL, "/", &save);
    }
    int readable = directory(fd, ".", 0);
    close(fd);
    if (readable < 0) return -1;
    fd = readable;
    struct stat st;
    if (fstat(fd, &st) || (create && (st.st_uid != geteuid() || (st.st_mode & 0022)))) {
        close(fd); return fail("storage root must be owned by this user and not writable by others");
    }
    if (!realpath(path, resolved)) { close(fd); return syserror("resolve root"); }
    return fd;
}

static int overlap(const char *a, const char *b) {
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == '/' || !b[n] || n == 1);
}

static int regular(int parent, const char *name, int flags, unsigned mode) {
    int fd = openat(parent, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
    if (fd < 0) return open_error("open file");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd); return fail("only regular files with one link are allowed");
    }
    return fd;
}

static int entry_open(int root, const char *path) {
    if (!app_path_valid(path)) return fail("unsafe relative path");
    char copy[APP_PATH_MAX];
    strcpy(copy, path);
    int fd = directory(root, ".", 0);
    if (fd < 0) return -1;
    char *save, *part = strtok_r(copy, "/", &save);
    for (;;) {
        char *next = strtok_r(NULL, "/", &save);
        int child = next ? directory(fd, part, 0) : regular(fd, part, O_RDONLY, 0);
        close(fd);
        if (child < 0 || !next) return child;
        fd = child; part = next;
    }
}

static int load_manifest(int root, const char *id, app_manifest *m) {
    int fd = regular(root, "app.json", O_RDONLY, 0);
    if (fd < 0) return -1;
    char text[APP_MANIFEST_MAX + 1];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof(text) && (n = read(fd, text + used, sizeof(text) - used)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd); return syserror("read manifest");
        }
        used += (size_t)n;
    }
    close(fd);
    if (app_manifest_parse(text, used, m, error, sizeof(error))) return -1;
    if (strcmp(id, m->id)) return fail("manifest id must match package directory");
    fd = entry_open(root, m->entry);
    if (fd < 0) return -1;
    int rc = app_elf_validate(fd, error, sizeof(error));
    close(fd);
    return rc;
}

static DIR *listing(int fd) {
    int copy = directory(fd, ".", 0);
    if (copy < 0) return NULL;
    DIR *d = fdopendir(copy);
    if (!d) { close(copy); syserror("read directory"); }
    return d;
}
static int compare_name(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static int write_all(int fd, const void *buffer, size_t length) {
    const unsigned char *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return syserror("write staged file");
        p += n; length -= (size_t)n;
    }
    return 0;
}
static void hash_number(app_sha256 *hash, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned i=0; i<8; ++i) bytes[i]=(unsigned char)(value>>(8*i));
    app_sha256_update(hash, bytes, sizeof(bytes));
}

typedef struct {
    app_sha256 hash;
    uint64_t bytes;
    unsigned nodes;
} tree_state;

/* Hash a sorted, framed tree; optionally copy the same opened files. Never follow links. */
static int tree_walk(int source, int dest, const char *prefix, unsigned depth, tree_state *state) {
    if (depth > MAX_DEPTH) return fail("package directory depth exceeded");
    DIR *d = listing(source);
    if (!d) return -1;
    char **names = calloc(MAX_NODES, sizeof(*names));
    if (!names) { closedir(d); return fail("out of memory"); }
    unsigned count = 0;
    int rc = -1;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count == MAX_NODES || !app_path_valid(e->d_name) || strchr(e->d_name, '/')) {
            fail("too many entries or non-portable package filename"); goto done;
        }
        names[count] = strdup(e->d_name);
        if (!names[count]) { fail("out of memory"); goto done; }
        ++count;
        errno = 0;
    }
    if (errno) { syserror("enumerate package"); goto done; }
    qsort(names, count, sizeof(*names), compare_name);
    for (unsigned i=0; i<count; ++i) {
        if (interrupted) { fail("interrupted"); goto done; }
        if (++state->nodes > MAX_NODES) { fail("package entry limit exceeded"); goto done; }
        char path[APP_PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", names[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) { fail("package path too long"); goto done; }
        struct stat st;
        if (fstatat(source, names[i], &st, AT_SYMLINK_NOFOLLOW)) { syserror("stat package"); goto done; }
        unsigned char type = S_ISDIR(st.st_mode) ? 'D' : 'F';
        app_sha256_update(&state->hash, &type, 1);
        hash_number(&state->hash, (uint64_t)length);
        app_sha256_update(&state->hash, path, (size_t)length);
        if (S_ISDIR(st.st_mode)) {
            int src = directory(source, names[i], 0);
            int dst = dest < 0 ? -1 : directory(dest, names[i], 1);
            if (src < 0 || (dest >= 0 && dst < 0)) {
                if (src >= 0) close(src);
                if (dst >= 0) close(dst);
                goto done;
            }
            int result = tree_walk(src, dst, path, depth + 1, state);
            close(src); if (dst >= 0) close(dst);
            if (result) goto done;
        } else if (S_ISREG(st.st_mode)) {
            int src = regular(source, names[i], O_RDONLY, 0);
            if (src < 0) goto done;
            if (fstat(src, &st) || st.st_size < 0 || (uint64_t)st.st_size > PACKAGE_BYTES - state->bytes) {
                close(src); fail("package size limit exceeded"); goto done;
            }
            uint64_t expected = (uint64_t)st.st_size, copied = 0;
            hash_number(&state->hash, expected);
            state->bytes += expected;
            int dst = dest < 0 ? -1 : regular(dest, names[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (dest >= 0 && dst < 0) { close(src); goto done; }
            unsigned char buffer[32768];
            ssize_t n;
            int failed = 0;
            for (;;) {
                if (interrupted) { fail("interrupted"); failed = 1; break; }
                n = read(src, buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { syserror("read package file"); failed = 1; break; }
                if (!n) break;
                copied += (uint64_t)n;
                if (copied > expected) { fail("package changed while being read"); failed = 1; break; }
                app_sha256_update(&state->hash, buffer, (size_t)n);
                if (dst >= 0 && write_all(dst, buffer, (size_t)n)) { failed = 1; break; }
            }
            if (!failed && copied != expected) { fail("package changed while being read"); failed = 1; }
            if (dst >= 0) {
                if (!failed && fsync(dst)) { syserror("sync staged file"); failed = 1; }
                if (close(dst) && !failed) { syserror("close staged file"); failed = 1; }
            }
            close(src);
            if (failed) goto done;
        } else { fail("links and special files are not allowed in packages"); goto done; }
    }
    rc = 0;
    if (dest >= 0 && fsync(dest)) rc = syserror("sync staged directory");
done:
    for (unsigned i=0; i<count; ++i) free(names[i]);
    free(names); closedir(d);
    return rc;
}

static int fingerprint(int source, int dest, char digest[65], uint64_t *bytes) {
    tree_state state = {0};
    app_sha256_init(&state.hash);
    static const char domain[] = "natartos-sd-package-v1";
    app_sha256_update(&state.hash, domain, sizeof(domain));
    if (tree_walk(source, dest, "", 0, &state)) return -1;
    app_sha256_final(&state.hash, digest);
    if (bytes) *bytes = state.bytes;
    return 0;
}

/* Removal is called only beneath the locked, runner-owned cache, never on SD/data roots. */
static int remove_tree(int parent, const char *name, unsigned depth) {
    if (depth > MAX_DEPTH + 3) return fail("cache removal depth exceeded");
    int fd = directory(parent, name, 0);
    if (fd < 0) return -1;
    DIR *d = listing(fd);
    if (!d) { close(fd); return -1; }
    int rc = 0;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) { rc = syserror("stat cache entry"); break; }
        if (S_ISDIR(st.st_mode)) rc = remove_tree(fd, e->d_name, depth + 1);
        else if (unlinkat(fd, e->d_name, 0)) rc = syserror("remove cache file");
        if (rc) break;
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate cache");
    closedir(d); close(fd);
    if (!rc && unlinkat(parent, name, AT_REMOVEDIR)) rc = syserror("remove cache directory");
    return rc;
}
static int digest_name(const char *name) {
    if (strlen(name) != 64) return 0;
    for (; *name; ++name) if (!strchr("0123456789abcdef", *name)) return 0;
    return 1;
}
static int clean_app(int app, int all) {
    DIR *d = listing(app);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, ".partial-", 9) || (all && digest_name(e->d_name))) {
            if (remove_tree(app, e->d_name, 0)) { rc = -1; break; }
        }
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate app cache");
    closedir(d);
    return rc;
}

static int cache_usage(int root, unsigned depth, uint64_t *bytes, unsigned *nodes) {
    if (depth > MAX_DEPTH + 3) return fail("unexpected cache depth");
    DIR *d = listing(root);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (++*nodes > 65536 || fstatat(root, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = fail("cannot measure cache or cache entry limit exceeded"); break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = directory(root, e->d_name, 0);
            if (child < 0) { rc = -1; break; }
            rc = cache_usage(child, depth + 1, bytes, nodes);
            close(child);
        } else if (S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= CACHE_BYTES - *bytes) {
            *bytes += (uint64_t)st.st_size;
        } else rc = fail("cache is full or contains an unexpected file");
        if (rc) break;
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate cache usage");
    closedir(d);
    return rc;
}

static int stage(int source, int packages, int app, const char *id, char digest[65], app_manifest *manifest) {
    uint64_t bytes;
    if (load_manifest(source, id, manifest) || fingerprint(source, -1, digest, &bytes) || clean_app(app, 0)) return -1;
    struct stat st;
    if (!fstatat(app, digest, &st, AT_SYMLINK_NOFOLLOW)) {
        int cached = directory(app, digest, 0);
        if (cached < 0) return -1;
        char actual[65];
        int rc = fingerprint(cached, -1, actual, NULL) || load_manifest(cached, id, manifest);
        close(cached);
        if (rc) return -1;
        if (strcmp(digest, actual)) return fail("cached package was modified; clean this app before retrying");
        return 0;
    }
    if (errno != ENOENT) return syserror("check cache");
    uint64_t used = 0;
    unsigned nodes = 0;
    if (cache_usage(packages, 0, &used, &nodes)) return -1;
    if (bytes > CACHE_BYTES - used) return fail("cache budget exceeded; clean inactive app versions");
    struct statvfs fs;
    if (fstatvfs(app, &fs)) return syserror("measure free storage");
    if (!fs.f_frsize || fs.f_bavail < (bytes + 2*1024*1024) / fs.f_frsize + 1)
        return fail("insufficient internal storage (2 MiB reserve required)");
    char partial[64];
    snprintf(partial, sizeof(partial), ".partial-%ld", (long)getpid());
    if (mkdirat(app, partial, 0700)) return syserror("create staging directory");
    int dst = directory(app, partial, 0);
    int rc = -1;
    char copied[65], verified[65];
    if (dst < 0) goto done;
    if (fingerprint(source, dst, copied, NULL) || strcmp(digest, copied)) {
        if (!*error) fail("source changed during copy");
        goto done;
    }
    if (fingerprint(dst, -1, verified, NULL) || strcmp(digest, verified)) {
        if (!*error) fail("staged copy verification failed");
        goto done;
    }
    if (load_manifest(dst, id, manifest)) goto done;
    int entry = entry_open(dst, manifest->entry);
    if (entry < 0) goto done;
    int mode_result = fchmod(entry, 0700) || fsync(entry);
    close(entry);
    if (mode_result) { syserror("set executable permissions"); goto done; }
    if (fsync(dst) || renameat(app, partial, app, digest) || fsync(app)) {
        syserror("publish staged package"); goto done;
    }
    rc = 0;
done:
    if (dst >= 0) close(dst);
    if (rc) {
        char original[sizeof(error)];
        strcpy(original, error);
        remove_tree(app, partial, 0);
        strcpy(error, original);
    }
    return rc;
}

static int64_t milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Display hand-off for apps whose manifest sets "display": true. This follows
 * the same protocol as the Doom and PC Stream launchers: while fb_override
 * exists, hook_overlay.so shows frames from fb_shared instead of the DJI UI;
 * removing the flag makes the overlay stop and repaint the DJI UI. The overlay
 * (or Toolya) asks for a stop by creating fb_stop, and its cleanup removes
 * fb_override itself.
 */
typedef struct {
    char override[PATH_MAX], stop[PATH_MAX], frame[PATH_MAX];
    const char *inputs;
    int owner, overlay, frame_fd;
    int claimed;
    int lost;   /* fb_override disappeared: the overlay already cleaned up */
} display_ctx;

static int display_overlay_idle(display_ctx *d) {
    if (flock(d->overlay, LOCK_EX | LOCK_NB))
        return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
    flock(d->overlay, LOCK_UN);
    return 1;
}

static int display_wait_idle(display_ctx *d) {
    int64_t until = milliseconds() + NAT_DISPLAY_WAIT_MS;
    /* Keep the lease once acquired, so a watcher cannot attach between the
     * stale-file check and publication of this session's new frame file. */
    while (flock(d->overlay, LOCK_EX | LOCK_NB)) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
        if (milliseconds() >= until) return -1;
        struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int present(const char *path) {
    struct stat st;
    return !fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT;
}

static int display_claim(display_ctx *d, const char *dir, const char *inputs) {
    memset(d, 0, sizeof(*d));
    d->owner = d->overlay = d->frame_fd = -1;
    d->inputs = inputs;
    if (snprintf(d->override, sizeof(d->override), "%s/fb_override", dir) >= (int)sizeof(d->override) ||
        snprintf(d->stop, sizeof(d->stop), "%s/fb_stop", dir) >= (int)sizeof(d->stop) ||
        snprintf(d->frame, sizeof(d->frame), "%s/fb_shared", dir) >= (int)sizeof(d->frame))
        return fail("display directory path too long");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/" NAT_DISPLAY_OWNER, dir);
    d->owner = regular(AT_FDCWD, path, O_RDWR | O_CREAT, 0600);
    if (d->owner < 0) return -1;
    if (flock(d->owner, LOCK_EX | LOCK_NB)) {
        /* Close this unlocked descriptor; release must not touch another owner. */
        close(d->owner); d->owner = -1;
        return fail("another display app runner is active");
    }
    snprintf(path, sizeof(path), "%s/" NAT_DISPLAY_OVERLAY, dir);
    d->overlay = regular(AT_FDCWD, path, O_RDWR | O_CREAT, 0600);
    if (d->overlay < 0) return -1;
    if (present(d->override)) {
        /* Only reclaim a recognisable SD session after acquiring its owner
         * lease. Empty legacy flags (Doom/PC Stream) remain busy. */
        int old = regular(AT_FDCWD, d->frame, O_RDONLY, 0);
        char magic[NAT_DISPLAY_MAGIC_BYTES];
        int ours = old >= 0 && pread(old, magic, sizeof(magic), NAT_DISPLAY_MAGIC_OFFSET) == sizeof(magic) &&
                   !memcmp(magic, NAT_DISPLAY_MAGIC, sizeof(magic));
        if (old >= 0) close(old);
        if (!ours) return fail("another display app is active (fb_override exists)");
        if (unlink(d->override) && errno != ENOENT) return syserror("release stale display");
    }
    if (display_wait_idle(d)) return fail("previous overlay cleanup did not finish");
    if (interrupted) return fail("interrupted while waiting for display");
    if (present(d->override)) return fail("another display app claimed the screen");
    /* A stale stop request would end this app as soon as the overlay attaches. */
    if (unlink(d->stop) && errno != ENOENT) return syserror("remove stale fb_stop");
    if (unlink(d->frame) && errno != ENOENT) return syserror("remove stale fb_shared");
    /* Frame file first: the overlay opens it as soon as it sees the flag. */
    int fd = open(d->frame, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return syserror("create fb_shared");
    /* Readable by the overlay whatever dji_glasses' uid; umask is 077 here. */
    if (fchmod(fd, 0644) || ftruncate(fd, (off_t)NAT_FB_FILE_BYTES)) {
        syserror("size fb_shared");
        close(fd); unlink(d->frame);
        return -1;
    }
    if (pwrite(fd, NAT_DISPLAY_MAGIC, NAT_DISPLAY_MAGIC_BYTES, NAT_DISPLAY_MAGIC_OFFSET) != NAT_DISPLAY_MAGIC_BYTES) {
        syserror("write display handshake"); close(fd); unlink(d->frame); return -1;
    }
    d->frame_fd = fd;
    fd = open(d->override, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        int saved = errno;
        unlink(d->frame);
        errno = saved;
        return syserror("claim display (fb_override)");
    }
    fchmod(fd, 0644);
    close(fd);
    d->claimed = 1;
    flock(d->overlay, LOCK_UN);
    int64_t until = milliseconds() + NAT_DISPLAY_WAIT_MS;
    while (!interrupted && milliseconds() < until) {
        unsigned ack = 0;
        if (pread(d->frame_fd, &ack, sizeof(ack), NAT_DISPLAY_ACK_OFFSET) == sizeof(ack) &&
            ack == NAT_DISPLAY_ACK_READY && display_overlay_idle(d) == 0) return 0;
        if (!present(d->override) || present(d->stop)) break;
        struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    return fail(interrupted ? "interrupted while waiting for overlay" :
                "overlay did not acknowledge display; start the updated Natartos overlay first");
}

/* Nonzero once the overlay or Toolya has asked the display app to stop. */
static int display_stop_requested(display_ctx *d) {
    if (!present(d->override)) { d->lost = 1; return 1; }
    return present(d->stop) || display_overlay_idle(d) != 0;
}

/* Give the screen back. Call only after the app's process group is gone, so
 * its input grab is released before the overlay injects the repaint key. */
static void display_release(display_ctx *d) {
    if (d->claimed) {
        if (!d->lost) unlink(d->override);
        /* Keep the owner lease while the overlay repaints. A subsequent app
         * must not have its files removed by this session's cleanup. */
        if (!display_wait_idle(d)) {
            unlink(d->stop);
            unlink(d->frame);
        }
    }
    if (d->frame_fd >= 0) close(d->frame_fd);
    if (d->overlay >= 0) close(d->overlay);
    if (d->owner >= 0) close(d->owner);
    d->owner = d->overlay = d->frame_fd = -1;
    d->claimed = 0;
}

/* Close every descriptor from 3 up except `keep`. The runner may be started
 * from dji_glasses (menu entry), whose open files must not leak into apps. */
static void close_inherited(int keep) {
#ifdef SYS_close_range
    if ((keep <= 3 || !syscall(SYS_close_range, 3u, (unsigned)keep - 1, 0u)) &&
        !syscall(SYS_close_range, (unsigned)(keep < 3 ? 3 : keep + 1), ~0u, 0u)) return;
#endif
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            long fd = strtol(e->d_name, &end, 10);
            if (*end || fd < 3 || fd == keep || fd == dirfd(d)) continue;
            close((int)fd);
        }
        closedir(d);
        return;
    }
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536) max = 65536;
    for (long fd = 3; fd < max; ++fd) if (fd != keep) close((int)fd);
}

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* `source` is the package directory the app was staged from (e.g. on the SD
 * card); the app runs from the verified internal copy at `package`. */
static int run_app(int package_fd, const char *package, const char *source, const char *data,
                   app_manifest *m, int logs, unsigned timeout, display_ctx *display) {
    int executable_fd = entry_open(package_fd, m->entry);
    if (executable_fd < 0) return -1;
    int log = regular(logs, "last-run.log", O_WRONLY | O_CREAT, 0600);
    if (log < 0) { close(executable_fd); return -1; }
    if (ftruncate(log, 0)) { close(executable_fd); close(log); return syserror("truncate app log"); }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { close(executable_fd); close(log); return syserror("create output pipe"); }
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) { close(executable_fd); close(log); close(pipefd[0]); close(pipefd[1]); return syserror("start application"); }
    if (!pid) {
        /* Parent death cannot leave the direct app running with an unlocked cache. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setpgid(0, 0)) _exit(126);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(pipefd[0]); close(log);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) close(input);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        char executable[PATH_MAX], package_env[PATH_MAX + 32], data_env[PATH_MAX + 32];
        char id_env[APP_ID_MAX + 32], fb_env[PATH_MAX + 32], input_env[PATH_MAX + 32];
        char source_env[PATH_MAX + 32];
        if (snprintf(executable, sizeof(executable), "%s/%s", package, m->entry) >= (int)sizeof(executable)) _exit(126);
        snprintf(package_env, sizeof(package_env), "NATARTOS_APP_PACKAGE=%s", package);
        snprintf(data_env, sizeof(data_env), "NATARTOS_APP_DATA=%s", data);
        snprintf(id_env, sizeof(id_env), "NATARTOS_APP_ID=%s", m->id);
        snprintf(source_env, sizeof(source_env), "NATARTOS_APP_SOURCE=%s", source);
        char *env[12];
        unsigned envc = 0;
#ifdef SD_APPS_HOST_TEST
        env[envc++] = "PATH=/usr/bin:/bin";
#else
        env[envc++] = "PATH=/system/bin:/system/xbin";
#endif
        env[envc++] = "LANG=C";
        env[envc++] = package_env;
        env[envc++] = data_env;
        env[envc++] = id_env;
        env[envc++] = source_env;
        if (display) {
            snprintf(fb_env, sizeof(fb_env), "NATARTOS_FB_PATH=%s", display->frame);
            snprintf(input_env, sizeof(input_env), "NATARTOS_INPUT_DEVICES=%s", display->inputs);
            env[envc++] = fb_env;
            env[envc++] = "NATARTOS_FB_WIDTH=" STRINGIFY(NAT_FB_WIDTH);
            env[envc++] = "NATARTOS_FB_HEIGHT=" STRINGIFY(NAT_FB_HEIGHT);
            env[envc++] = input_env;
        }
        env[envc] = NULL;
        char *argv[APP_ARGS_MAX + 2];
        argv[0] = executable;
        for (unsigned i=0; i<m->argc; ++i) argv[i+1] = m->args[i];
        argv[m->argc+1] = NULL;
        if (fchdir(package_fd)) { dprintf(2, "fchdir: %s\n", strerror(errno)); _exit(126); }
        close_inherited(executable_fd);
        /* Execute the opened staged inode, without resolving the absolute path again. */
        fexecve(executable_fd, argv, env);
        dprintf(2, "fexecve: %s\n", strerror(errno));
        _exit(127);
    }
    close(executable_fd);
    close(pipefd[1]);
    setpgid(pid, pid); /* Child also sets its group before exec; close the scheduling race. */
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    int status = 0, exited = 0, stopping = 0, result = 0, log_failed = 0;
    size_t logged = 0;
    int64_t start = milliseconds(), kill_at = 0;
    while (!exited) {
        int64_t now = milliseconds();
        if (!stopping && (interrupted || (timeout && now - start >= (int64_t)timeout*1000))) {
            result = interrupted ? 128 + interrupted : 124;
            kill(-pid, SIGTERM); stopping = 1; kill_at = now + 2000;
        }
        /* A stop request (fb_stop from Toolya/SIGUSR1, or the overlay's own
         * cleanup removing fb_override) ends the app. The overlay injects its
         * repaint key ~0.45 s later, so the app's input grab must go quickly. */
        if (!stopping && display && display_stop_requested(display)) {
            result = 128 + SIGTERM;
            kill(-pid, SIGTERM); stopping = 1; kill_at = now + 2000;
        }
        if (stopping && now >= kill_at) { kill(-pid, SIGKILL); kill_at = INT64_MAX; }
        struct pollfd p = {pipefd[0], POLLIN, 0};
        poll(&p, 1, 50);
        if (p.revents & (POLLHUP | POLLERR)) {
            struct timespec pause = {0, 50000000};
            nanosleep(&pause, NULL);
        }
        unsigned char buffer[4096];
        /* Limit each drain pass, including when an app writes continuously. */
        for (unsigned i=0; i<16; ++i) {
            ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n <= 0) break;
            size_t keep = (size_t)n;
            if (keep > LOG_BYTES - logged) keep = LOG_BYTES - logged;
            if (keep && write_all(log, buffer, keep)) {
                log_failed = 1;
                if (!stopping) { kill(-pid, SIGTERM); stopping = 1; kill_at = now + 2000; }
            }
            logged += keep;
        }
        siginfo_t child = {0};
        /* Reserve the leader PID until group cleanup, avoiding PID/PGID reuse. */
        int waited = waitid(P_PID, (id_t)pid, &child, WEXITED | WNOHANG | WNOWAIT);
        if (!waited && child.si_pid == pid) exited = 1;
        else if (waited < 0 && errno != EINTR) {
            syserror("wait for application"); log_failed = 1;
            kill(-pid, SIGKILL); exited = 1;
        }
    }
    /* Descendants do not get to keep using the package after their leader exits. */
    kill(-pid, SIGTERM);
    struct timespec grace = {0, 100000000};
    nanosleep(&grace, NULL);
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        syserror("reap application"); log_failed = 1; break;
    }
    /* Drain remaining output, but never wait for a detached process to close the pipe. */
    unsigned char tail[4096];
    for (unsigned i=0; i<256; ++i) {
        ssize_t n = read(pipefd[0], tail, sizeof(tail));
        if (n <= 0) break;
        size_t keep = (size_t)n;
        if (keep > LOG_BYTES - logged) keep = LOG_BYTES - logged;
        if (keep && write_all(log, tail, keep)) log_failed = 1;
        logged += keep;
    }
    close(pipefd[0]);
    if (fsync(log)) { syserror("sync app log"); log_failed = 1; }
    close(log);
    if (log_failed) return -1;
    if (!result) result = WIFEXITED(status) ? WEXITSTATUS(status) :
                         WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    printf("exit=%d log_bytes=%zu\n", result, logged);
    return result;
}

static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static int list_apps(int apps) {
    DIR *d = listing(apps);
    if (!d) return -1;
    struct dirent *e;
    unsigned count = 0;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!app_id_valid(e->d_name)) continue;
        if (++count > 128) { rc = fail("app catalog exceeds 128 entries"); break; }
        int root = directory(apps, e->d_name, 0);
        app_manifest m;
        char digest[65];
        if (root < 0 || load_manifest(root, e->d_name, &m) || fingerprint(root, -1, digest, NULL)) {
            fprintf(stderr, "skip %s: %s\n", e->d_name, error);
        } else {
            fputs("{\"id\":", stdout); json_string(m.id);
            fputs(",\"name\":", stdout); json_string(m.name);
            fputs(",\"version\":", stdout); json_string(m.version);
            fputs("}\n", stdout);
        }
        if (root >= 0) close(root);
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate apps");
    closedir(d);
    return rc;
}

/*
 * Listing for mod_sd_apps.so, which registers one More-menu entry per app
 * while dji_glasses starts. Lines are "id<TAB>version<TAB>name", sorted by
 * name. Checks manifests and executables only (no full-package hash), so a
 * large package cannot hold up dji_glasses start-up; launch re-verifies.
 */
typedef struct {
    char id[APP_ID_MAX + 1], version[65], name[APP_TEXT_MAX + 1];
} menu_item;

/* The menu builds labels with EwNewStringAnsi: keep printable ASCII. */
static void ascii_label(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && n + 1 < size; ++s) {
        if ((*s & 0xc0) == 0x80) continue;
        out[n++] = (*s >= 32 && *s < 127) ? (char)*s : '?';
    }
    out[n] = 0;
}

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int compare_item(const void *a, const void *b) {
    const menu_item *x = a, *y = b;
    for (const char *p = x->name, *q = y->name;; ++p, ++q) {
        int d = fold((unsigned char)*p) - fold((unsigned char)*q);
        if (d) return d;
        if (!*p) break;
    }
    return strcmp(x->id, y->id);
}

static int menu_list(int apps) {
    DIR *d = listing(apps);
    if (!d) return -1;
    menu_item *items = calloc(MENU_LIST_MAX, sizeof(*items));
    if (!items) { closedir(d); return fail("out of memory"); }
    unsigned count = 0;
    int rc = 0;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!app_id_valid(e->d_name)) continue;
        if (count == MENU_LIST_MAX) {
            fprintf(stderr, "menu-list: more than %d apps; the rest are not listed\n", MENU_LIST_MAX);
            break;
        }
        int root = directory(apps, e->d_name, 0);
        app_manifest m;
        if (root < 0 || load_manifest(root, e->d_name, &m)) {
            fprintf(stderr, "skip %s: %s\n", e->d_name, error);
        } else {
            menu_item *item = &items[count++];
            snprintf(item->id, sizeof(item->id), "%s", m.id);
            snprintf(item->version, sizeof(item->version), "%s", m.version);
            ascii_label(item->name, sizeof(item->name), m.name);
        }
        if (root >= 0) close(root);
        errno = 0;
    }
    if (!e && errno) rc = syserror("enumerate apps");
    closedir(d);
    if (!rc) {
        qsort(items, count, sizeof(*items), compare_item);
        for (unsigned i = 0; i < count; ++i)
            printf("%s\t%s\t%s\n", items[i].id, items[i].version, items[i].name);
    }
    free(items);
    return rc;
}

static int discover(char result[PATH_MAX]) {
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts) return syserror("read mount table");
    struct mntent *m;
    unsigned matches = 0;
    while ((m = getmntent(mounts))) {
        if (strncmp(m->mnt_dir, "/mnt/media_rw/", 14) || !strcmp(m->mnt_dir, "/mnt/media_rw/emulated")) continue;
        if (strcmp(m->mnt_type, "exfat") && strcmp(m->mnt_type, "vfat")) continue;
        if (snprintf(result, PATH_MAX, "%s/natartos/apps", m->mnt_dir) >= PATH_MAX) {
            endmntent(mounts); return fail("SD mount path too long");
        }
        ++matches;
    }
    endmntent(mounts);
    if (matches != 1) return fail("expected one external FAT/exFAT mount; specify --apps explicitly");
    return 0;
}

static void usage(void) {
    puts("sd_app_runner [--apps ABS] [--cache ABS] [--data ABS] [--timeout SECONDS]\n"
         "              [--display-dir ABS] [--input ABS]...\n"
         "              list | menu-list | validate ID | stage ID | run ID | clean ID\n"
         "Defaults: external SD/natartos/apps; cache " CACHE_DEFAULT "; data " DATA_DEFAULT ";\n"
         "display flags in " DISPLAY_DEFAULT "; input " NAT_INPUT_DEFAULT ".\n"
         "Static ARM64 apps only. Host-test builds accept their build architecture.\n"
         "Exit: app status, 124 timeout, 125 runner error, 128+signal cancellation.");
}

/* Started from dji_glasses' system(), stdio may be closed; keep 0-2 occupied
 * so no runner file or pipe ends up as an app's stdin/stdout/stderr. */
static void ensure_stdio(void) {
    int fd;
    while ((fd = open("/dev/null", O_RDWR | O_CLOEXEC)) >= 0 && fd <= STDERR_FILENO)
        fcntl(fd, F_SETFD, 0);
    if (fd > STDERR_FILENO) close(fd);
}

int main(int argc, char **argv) {
    ensure_stdio();
    const char *apps_path = NULL, *cache_path = CACHE_DEFAULT, *data_path = DATA_DEFAULT;
    const char *display_dir = DISPLAY_DEFAULT;
    char inputs[NAT_INPUT_MAX * 256] = "";
    unsigned timeout = 0, input_count = 0;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (i+1 == argc) { usage(); return 125; }
        if (!strcmp(argv[i], "--apps")) apps_path = argv[i+1];
        else if (!strcmp(argv[i], "--cache")) cache_path = argv[i+1];
        else if (!strcmp(argv[i], "--data")) data_path = argv[i+1];
        else if (!strcmp(argv[i], "--display-dir")) {
            if (*argv[i+1] != '/' || strlen(argv[i+1]) >= PATH_MAX - 16) { usage(); return 125; }
            display_dir = argv[i+1];
        } else if (!strcmp(argv[i], "--input")) {
            /* Passed to display apps as a colon-separated list. */
            const char *path = argv[i+1];
            if (*path != '/' || strchr(path, ':') || strlen(path) >= 255 || input_count == NAT_INPUT_MAX) {
                usage(); return 125;
            }
            if (input_count++) strcat(inputs, ":");
            strcat(inputs, path);
        } else if (!strcmp(argv[i], "--timeout")) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(argv[i+1], &end, 10);
            if (errno || !*argv[i+1] || *end || n > 86400 || *argv[i+1] == '-') { usage(); return 125; }
            timeout = (unsigned)n;
        } else { usage(); return 125; }
    }
    if (!input_count) strcpy(inputs, NAT_INPUT_DEFAULT);
    if (i >= argc) { usage(); return 125; }
    const char *action = argv[i++], *id = i < argc ? argv[i++] : NULL;
    int listing_action = !strcmp(action, "list") || !strcmp(action, "menu-list");
    if (i != argc || (!listing_action && strcmp(action, "validate") &&
        strcmp(action, "stage") && strcmp(action, "run") && strcmp(action, "clean")) ||
        (listing_action ? id != NULL : !id || !app_id_valid(id))) {
        usage(); return 125;
    }
    /* menu-list runs inside dji_glasses start-up; never let a slow card stall it. */
    if (!strcmp(action, "menu-list")) alarm(MENU_LIST_SECONDS);
    umask(0077);
    struct sigaction sa = {0};
    signal(SIGCHLD, SIG_DFL);
    sa.sa_handler = on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    char discovered[PATH_MAX], apps_abs[PATH_MAX], cache_abs[PATH_MAX], data_abs[PATH_MAX];
    int apps = -1, source = -1, cache = -1, lock = -1, packages = -1, app = -1, data = -1, logs = -1, staged = -1;
    int rc = -1;
    app_manifest manifest;
    if (strcmp(action, "clean")) {
        if (!apps_path) {
            if (discover(discovered)) goto done;
            apps_path = discovered;
        }
        apps = root_directory(apps_path, 0, apps_abs);
        if (apps < 0) goto done;
        if (!strcmp(action, "list")) { rc = list_apps(apps); goto done; }
        if (!strcmp(action, "menu-list")) { rc = menu_list(apps); goto done; }
        source = directory(apps, id, 0);
        if (source < 0) goto done;
        if (!strcmp(action, "validate")) {
            char digest[65];
            uint64_t bytes;
            if (load_manifest(source, id, &manifest) || fingerprint(source, -1, digest, &bytes)) goto done;
            printf("valid id=%s sha256=%s bytes=%llu\n", id, digest, (unsigned long long)bytes);
            rc = 0; goto done;
        }
    }
    cache = root_directory(cache_path, 1, cache_abs);
    if (cache < 0) goto done;
    if (apps >= 0 && (overlap(apps_abs, cache_abs) || overlap(cache_abs, apps_abs))) {
        fail("source and cache roots must be separate"); goto done;
    }
    lock = regular(cache, ".lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0) goto done;
    if (flock(lock, LOCK_EX | LOCK_NB)) { fail("another app runner is active"); goto done; }
    packages = directory(cache, "packages", 1);
    if (packages < 0) goto done;
    app = directory(packages, id, 1);
    if (app < 0) goto done;
    if (!strcmp(action, "clean")) { rc = clean_app(app, 1); goto done; }
    char digest[65];
    if (stage(source, packages, app, id, digest, &manifest)) goto done;
    char package[PATH_MAX];
    if (snprintf(package, sizeof(package), "%s/packages/%s/%s", cache_abs, id, digest) >= (int)sizeof(package)) {
        fail("cache path too long"); goto done;
    }
    if (!strcmp(action, "stage")) { puts(package); rc = 0; goto done; }
    data = root_directory(data_path, 1, data_abs);
    if (data < 0) goto done;
    if (overlap(cache_abs, data_abs) || overlap(data_abs, cache_abs) ||
        overlap(apps_abs, data_abs) || overlap(data_abs, apps_abs)) {
        fail("source, cache and data roots must be separate"); goto done;
    }
    int app_data = directory(data, id, 1);
    if (app_data < 0) goto done;
    close(app_data);
    char save[PATH_MAX], source_path[PATH_MAX];
    if (snprintf(save, sizeof(save), "%s/%s", data_abs, id) >= (int)sizeof(save)) { fail("data path too long"); goto done; }
    if (snprintf(source_path, sizeof(source_path), "%s/%s", apps_abs, id) >= (int)sizeof(source_path)) {
        fail("source path too long"); goto done;
    }
    logs = directory(cache, "logs", 1);
    if (logs < 0) goto done;
    staged = directory(app, digest, 0);
    if (staged < 0) goto done;
    display_ctx display = {.owner = -1, .overlay = -1, .frame_fd = -1};
    /* Claim the screen only once the verified copy is ready to execute. */
    if (manifest.display && display_claim(&display, display_dir, inputs)) {
        display_release(&display); goto done;
    }
    fflush(stdout);
    rc = run_app(staged, package, source_path, save, &manifest, logs, timeout,
                 manifest.display ? &display : NULL);
    display_release(&display);
done:
    if (rc < 0) fprintf(stderr, "sd_app_runner: %s\n", *error ? error : "operation failed");
    int fds[] = {staged, logs, data, app, packages, lock, cache, source, apps};
    for (unsigned k=0; k<sizeof(fds)/sizeof(*fds); ++k) if (fds[k] >= 0) close(fds[k]);
    return rc < 0 ? (interrupted ? 128 + interrupted : 125) :
           !rc && interrupted ? 128 + interrupted : rc;
}
