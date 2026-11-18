/* SPDX-License-Identifier: AGPL-3.0-only
 * Experimental marker-only proxy for the getter-based GPS ABI.
 * No firmware, constructors, or interfaces are copied from a vendor library.
 */
#include <stddef.h>
#include <stdint.h>
#include "raw_syscall.h"
#ifdef NATARTOS_BOOT_ACTIVATION
#include "../boot_activate/trigger.h"
#endif

#ifdef NATARTOS_BOOT_PROBE_TEST
#define STOCK_PATH "./stock.so"
#define MARKER_PATH "./marker"
#else
#define STOCK_PATH "/vendor/lib64/gps.default.stock.so"
#define MARKER_PATH "/data/dji/natartos_boot_probe.log"
#endif

/* The only dynamic imports; resolved through the explicit libdl.so dependency. */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

typedef const void *(*gps_getter)(void *);
static gps_getter stock_getter;

__attribute__((visibility("default")))
const void *gps_get_hardware_interface(void *device) {
    gps_getter getter = __atomic_load_n(&stock_getter, __ATOMIC_ACQUIRE);
    return getter ? getter(device) : NULL;
}

enum { O_WRONLY_ = 1, O_APPEND_ = 02000, O_NONBLOCK_ = 04000,
       O_NOFOLLOW_ = RAW_O_NOFOLLOW, O_CLOEXEC_ = 02000000,
       AT_EMPTY_PATH_ = 0x1000, STATX_TYPE_ = 1, STATX_MODE_ = 2, STATX_UID_ = 8, STATX_SIZE_ = 0x200,
       LOCK_EX_ = 2, LOCK_NB_ = 4, LOG_LIMIT = 65536 };

/* Linux statx has one fixed layout on both supported architectures. */
struct probe_statx {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad;
    uint64_t ino, size;
    unsigned char rest[208];
};
_Static_assert(sizeof(struct probe_statx) == 256, "statx ABI");
_Static_assert(offsetof(struct probe_statx, mode) == 28, "statx mode ABI");
_Static_assert(offsetof(struct probe_statx, size) == 40, "statx size ABI");

struct record { char data[1536]; size_t used; };
static void text(struct record *r, const char *s) {
    while (*s && r->used + 1 < sizeof(r->data)) r->data[r->used++] = *s++;
}
static void number(struct record *r, long value) {
    char digits[24]; unsigned count = 0;
    unsigned long n;
    if (value < 0) { text(r, "-"); n = 0UL - (unsigned long)value; }
    else n = (unsigned long)value;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count && r->used + 1 < sizeof(r->data)) r->data[r->used++] = digits[--count];
}
static void clean(struct record *r, const char *s, size_t length) {
    for (size_t i = 0; i < length && s[i] && r->used + 1 < sizeof(r->data); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r') break;
        r->data[r->used++] = c > 32 && c < 127 ? (char)c : '_';
    }
}
static long read_once(const char *path, char *out, size_t length) {
    long fd = raw_open(path, O_CLOEXEC_ | O_NOFOLLOW_ | O_NONBLOCK_);
    if (fd < 0) return fd;
    long result = raw_call(NR_read, fd, (long)out, (long)length, 0, 0);
    raw_close(fd);
    return result;
}
static void diagnostic(struct record *r, const char *key, const char *path) {
    char buffer[256];
    text(r, key);
    long length = read_once(path, buffer, sizeof(buffer));
    if (length > 0) clean(r, buffer, (size_t)length);
    else { text(r, "unreadable:"); number(r, length); }
}
static void capabilities(struct record *r) {
    char status[4096];
    long length = read_once("/proc/self/status", status, sizeof(status));
    text(r, " CapEff=");
    if (length > 0) {
        const char key[] = "CapEff:\t";
        for (size_t i = 0; i + sizeof(key) - 1 < (size_t)length; ++i) {
            if (i && status[i - 1] != '\n') continue;
            size_t j = 0;
            while (j < sizeof(key) - 1 && status[i + j] == key[j]) ++j;
            if (j == sizeof(key) - 1) {
                clean(r, status + i + j, (size_t)length - i - j);
                return;
            }
        }
    }
    text(r, "unreadable:"); number(r, length);
}

static void marker(const char *stock_status) {
    /* Never create/truncate the marker, follow its final symlink, or wait for
     * another probe. Marker failure must not change the forwarding result. */
    long fd = raw_open(MARKER_PATH, O_WRONLY_ | O_APPEND_ | O_CLOEXEC_ |
                                   O_NOFOLLOW_ | O_NONBLOCK_);
    if (fd < 0) return;
    struct probe_statx st;
    long rc = raw_call(NR_statx, fd, (long)"", AT_EMPTY_PATH_,
                       STATX_TYPE_ | STATX_MODE_ | STATX_UID_ | STATX_SIZE_, (long)&st);
    unsigned required = STATX_TYPE_ | STATX_MODE_ | STATX_UID_ | STATX_SIZE_;
    if (rc < 0 || (st.mask & required) != required || (st.mode & 0170000) != 0100000 ||
        st.uid != (uint32_t)raw_call(NR_geteuid, 0, 0, 0, 0, 0) ||
        (st.mode & 0022) || st.size >= LOG_LIMIT) { raw_close(fd); return; }
    if (raw_call(NR_flock, fd, LOCK_EX_ | LOCK_NB_, 0, 0, 0) < 0) { raw_close(fd); return; }
    /* Recheck size after acquiring the nonblocking lock. */
    rc = raw_call(NR_statx, fd, (long)"", AT_EMPTY_PATH_, STATX_SIZE_, (long)&st);
    if (rc < 0 || !(st.mask & STATX_SIZE_)) { raw_close(fd); return; }
    struct record r;
    r.used = 0;
    text(&r, "natartos_boot_probe v=1 phase=load pid=");
    number(&r, raw_call(NR_getpid, 0, 0, 0, 0, 0));
    text(&r, " uid="); number(&r, raw_call(NR_getuid, 0, 0, 0, 0, 0));
    text(&r, " euid="); number(&r, raw_call(NR_geteuid, 0, 0, 0, 0, 0));
    text(&r, " gid="); number(&r, raw_call(NR_getgid, 0, 0, 0, 0, 0));
    text(&r, " egid="); number(&r, raw_call(NR_getegid, 0, 0, 0, 0, 0));
    text(&r, " stock="); text(&r, stock_status);
    diagnostic(&r, " context=", "/proc/self/attr/current");
    capabilities(&r);
    diagnostic(&r, " enforcing=", "/sys/fs/selinux/enforce");
    diagnostic(&r, " boot_id=", "/proc/sys/kernel/random/boot_id");
    r.data[r.used++] = '\n';
    if (st.size <= LOG_LIMIT && r.used <= LOG_LIMIT - st.size)
        (void)raw_call(NR_write, fd, (long)r.data, (long)r.used, 0, 0);
    /* One bounded write: no retry loop, fsync, or background process. */
    raw_close(fd);
}

__attribute__((constructor))
static void boot_probe_init(void) {
    /* Keep the successful dlopen reference for the process lifetime: the stock
     * interface and its callback pointers must remain mapped after we return. */
    void *handle = dlopen(STOCK_PATH, 2 /* RTLD_NOW | RTLD_LOCAL */);
    const char *status = "open-failed";
    if (handle) {
        gps_getter getter = (gps_getter)dlsym(handle, "gps_get_hardware_interface");
        if (!getter) status = "symbol-missing";
        else if (getter == gps_get_hardware_interface) status = "self-reference";
        else {
            __atomic_store_n(&stock_getter, getter, __ATOMIC_RELEASE);
            status = "ready";
        }
    }
    marker(status);
#ifdef NATARTOS_BOOT_ACTIVATION
    if (__atomic_load_n(&stock_getter, __ATOMIC_ACQUIRE)) boot_activation_start();
#endif
}
