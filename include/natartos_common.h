/*
 * natartos_common.h — shared utilities for all natartos mods.
 * All functions are static inline — no runtime dependency, no .so needed.
 * Include from any mod: #include "natartos_common.h"
 */

#ifndef NATARTOS_COMMON_H
#define NATARTOS_COMMON_H

/* --- Constants --- */

#define AT_FDCWD    (-100)
#define PROT_READ   1
#define PROT_WRITE  2
#define PAGE_SIZE   4096
#define RTLD_DEFAULT ((void *)0)
#define F_OK        0
#define R_OK        4

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0100
#define O_APPEND    02000
#define O_TRUNC     01000
#define O_LARGEFILE 0400000

/* ARM64 syscall numbers */
#define __NR_openat     56
#define __NR_read       63
#define __NR_write      64
#define __NR_close      57
#define __NR_lseek      62
#define __NR_nanosleep  101
#define __NR_mprotect   226
#define __NR_faccessat  48

struct timespec_t { long tv_sec; long tv_nsec; };

/* --- Syscall wrappers --- */

static inline long sys_openat(int dirfd, const char *path, int flags, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = __NR_openat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long sys_write_raw(int fd, const void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_write;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = __NR_close;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long sys_mprotect(void *addr, long len, int prot) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x8 __asm__("x8") = __NR_mprotect;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_faccessat(int dirfd, const char *path, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = mode;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = __NR_faccessat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long sys_read(int fd, void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_read;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_lseek(int fd, long offset, int whence) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = offset;
    register long x2 __asm__("x2") = whence;
    register long x8 __asm__("x8") = __NR_lseek;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline void sys_nanosleep_ms(int ms) {
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000L };
    register long x0 __asm__("x0") = (long)ts;
    register long x1 __asm__("x1") = 0;
    register long x8 __asm__("x8") = __NR_nanosleep;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
}

static inline long sys_nanosleep(const struct timespec_t *req, struct timespec_t *rem) {
    register long x0 __asm__("x0") = (long)req;
    register long x1 __asm__("x1") = (long)rem;
    register long x8 __asm__("x8") = __NR_nanosleep;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

/* --- String helpers --- */

static inline int natartos_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int natartos_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline void natartos_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static inline void natartos_memset(void *dst, int c, long n) {
    unsigned char *p = (unsigned char *)dst;
    while (n-- > 0) *p++ = (unsigned char)c;
}

static inline void natartos_memcpy(void *dst, const void *src, long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n-- > 0) *d++ = *s++;
}

/* --- Unified logging ---
 *
 * All natartos logs go to one file. Mods define NTLOG_PREFIX before including
 * this header, then use NTLOG/NTLOG_HEX/NTLOG_INT. Output: "[prefix] msg\n".
 * Single write() per line — atomic under O_APPEND for size < PIPE_BUF.
 */

#define NATARTOS_LOG_PATH "/data/local/tmp/natartos.log"

static inline void natartos_log_raw(const char *prefix, const char *msg) {
    char buf[256];
    int i = 0;
    buf[i++] = '[';
    const char *p = prefix;
    while (*p && i < 18) buf[i++] = *p++;
    buf[i++] = ']';
    buf[i++] = ' ';
    const char *m = msg;
    while (*m && i < 254) buf[i++] = *m++;
    buf[i++] = '\n';
    int fd = (int)sys_openat(AT_FDCWD, NATARTOS_LOG_PATH,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sys_write_raw(fd, buf, i);
        sys_close(fd);
    }
}

static inline void natartos_log_raw_hex(const char *prefix, const char *label, unsigned long val) {
    char msg[80];
    int i = 0;
    const char *l = label;
    while (*l && i < 50) msg[i++] = *l++;
    msg[i++] = '0'; msg[i++] = 'x';
    int started = 0;
    for (int j = 60; j >= 0; j -= 4) {
        int n = (val >> j) & 0xf;
        if (!started && n == 0 && j > 0) continue;
        started = 1;
        msg[i++] = n < 10 ? '0' + n : 'a' + n - 10;
    }
    if (!started) msg[i++] = '0';
    msg[i] = 0;
    natartos_log_raw(prefix, msg);
}

static inline void natartos_log_raw_int(const char *prefix, const char *label, int val) {
    char msg[80];
    int i = 0;
    const char *l = label;
    while (*l && i < 50) msg[i++] = *l++;
    if (val < 0) { msg[i++] = '-'; val = -val; }
    char digits[12];
    int d = 0;
    if (val == 0) digits[d++] = '0';
    while (val > 0) { digits[d++] = '0' + val % 10; val /= 10; }
    while (d > 0) msg[i++] = digits[--d];
    msg[i] = 0;
    natartos_log_raw(prefix, msg);
}

/* Convenience macros — define NTLOG_PREFIX before including this header */
#ifdef NTLOG_PREFIX
#define NTLOG(msg)           natartos_log_raw(NTLOG_PREFIX, (msg))
#define NTLOG_HEX(label, v)  natartos_log_raw_hex(NTLOG_PREFIX, (label), (v))
#define NTLOG_INT(label, v)  natartos_log_raw_int(NTLOG_PREFIX, (label), (v))
#endif

/* Legacy API — writes to a specific path, no prefix */
static inline void natartos_log_to(const char *path, const char *msg) {
    int fd = (int)sys_openat(AT_FDCWD, path,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sys_write_raw(fd, msg, natartos_strlen(msg));
        sys_write_raw(fd, "\n", 1);
        sys_close(fd);
    }
}

/* --- vtable patching --- */

static inline int natartos_patch_vmt(void **vmt, int slot, void *new_fn, void **out_orig) {
    if (!vmt || !vmt[slot]) return -1;
    *out_orig = vmt[slot];

    unsigned long page = (unsigned long)&vmt[slot] & ~(PAGE_SIZE - 1);
    long ret = sys_mprotect((void *)page, PAGE_SIZE * 2, PROT_READ | PROT_WRITE);
    if (ret < 0) return -2;

    vmt[slot] = new_fn;
    sys_mprotect((void *)page, PAGE_SIZE * 2, PROT_READ);
    return 0;
}

/* --- Flag file check --- */

static inline int natartos_flag_exists(const char *path) {
    return sys_faccessat(AT_FDCWD, path, F_OK) == 0;
}

/* --- Menu registration API --- */

typedef struct {
    const char *id;           /* unique key, e.g. "doom" */
    const char *name;         /* display name */
    const char *subtitle;     /* second line, or NULL */
    const char *icon_path;    /* NC01 .rgba file, or NULL for fallback */
    const char *command;      /* shell command on tap, or NULL */
    void (*on_action)(void);  /* callback on tap (used if command==NULL) */
    int priority;             /* sort order: lower = higher in menu */
} natartos_menu_entry;

/* More-menu entries across all mods (hook_menu keeps one ~1.2 KB slot each).
 * The More menu is a scrolling list; SD Apps adds one entry per card app. */
#define NATARTOS_MAX_MENU_ITEMS 32

typedef int (*natartos_menu_register_fn)(const natartos_menu_entry *entry);

#endif /* NATARTOS_COMMON_H */
