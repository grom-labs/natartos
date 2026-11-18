/*
 * natartos_loader.so — anti-brick mod loader for DJI Goggles 3.
 *
 * The only .so in dji_glasses' LD_PRELOAD.
 * Reads the boot counter, decides whether to load mods, starts the health thread.
 *
 * Build: aarch64-linux-gnu-gcc -shared -fPIC -O2 -fno-builtin -nostdlib
 *        -Wl,-soname,natartos_loader.so -o natartos_loader.so natartos_loader.c
 */

#ifdef NATARTOS_BOOT_ACTIVATION
#include "boot_activate/loader_health.h"
#endif

/* ── Constants ── */

#define AT_FDCWD      (-100)
#define O_RDONLY      0
#define O_WRONLY      1
#define O_RDWR        2
#define O_CREAT       0100
#define O_TRUNC       01000
#define O_APPEND      02000
#define RTLD_NOW      0x00002
#define RTLD_LAZY     0x00001
#define RTLD_GLOBAL   0x00100
#define RTLD_DEFAULT  ((void *)0)

#define SAFE_THRESHOLD   5
#define HEALTH_DELAY_SEC 30

#define BOOTCOUNT_PATH  "/data/.natartos_bootcount"
#define DISABLED_PATH   "/data/.natartos_disabled"
#define STATUS_PATH     "/data/.natartos_status"
#define MODS_DIR        "/vendor/natartos/mods"
#define LOG_PATH        "/data/local/tmp/natartos.log"

/* ── Syscall numbers (aarch64) ── */

#define __NR_openat      56
#define __NR_close        57
#define __NR_read         63
#define __NR_write        64
#define __NR_faccessat    48
#define __NR_getdents64   61
#define __NR_nanosleep   101

struct timespec_t { long tv_sec; long tv_nsec; };

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

/* ── Raw syscalls ── */

static long sys_openat(int dirfd, const char *path, int flags, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = __NR_openat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static long sys_read(int fd, void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_read;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long sys_write(int fd, const void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_write;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = __NR_close;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long sys_faccessat(int dirfd, const char *path, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = mode;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = __NR_faccessat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static long sys_getdents64(int fd, void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_getdents64;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long sys_nanosleep(const struct timespec_t *req, struct timespec_t *rem) {
    register long x0 __asm__("x0") = (long)req;
    register long x1 __asm__("x1") = (long)rem;
    register long x8 __asm__("x8") = __NR_nanosleep;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

/* ── Minimal string helpers ── */

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int ends_with_so(const char *s) {
    int len = my_strlen(s);
    if (len < 3) return 0;
    return s[len-3] == '.' && s[len-2] == 's' && s[len-1] == 'o';
}

/* ── Logging ── */

#define LOG_PREFIX "[loader] "
#define LOG_PREFIX_LEN 9

static int g_log_fd = -1;

static void log_open(void) {
    g_log_fd = (int)sys_openat(AT_FDCWD, LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static void log_line(const char *msg) {
    if (g_log_fd < 0) return;
    char buf[256];
    int i = 0;
    const char *p = LOG_PREFIX;
    while (*p && i < LOG_PREFIX_LEN) buf[i++] = *p++;
    while (*msg && i < 253) buf[i++] = *msg++;
    buf[i++] = '\n';
    sys_write(g_log_fd, buf, i);
}

static void log_line2(const char *a, const char *b) {
    char tmp[240];
    int i = 0;
    while (*a && i < 180) tmp[i++] = *a++;
    while (*b && i < 238) tmp[i++] = *b++;
    tmp[i] = 0;
    log_line(tmp);
}

static void log_line3(const char *a, const char *b, const char *c) {
    char tmp[240];
    int i = 0;
    while (*a && i < 140) tmp[i++] = *a++;
    while (*b && i < 200) tmp[i++] = *b++;
    while (*c && i < 238) tmp[i++] = *c++;
    tmp[i] = 0;
    log_line(tmp);
}

static void log_close(void) {
    if (g_log_fd >= 0) sys_close(g_log_fd);
    g_log_fd = -1;
}

/* ── Boot counter ── */

static unsigned int read_bootcount(void) {
    unsigned int count = 0;
    int fd = (int)sys_openat(AT_FDCWD, BOOTCOUNT_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    sys_read(fd, &count, 4);
    sys_close(fd);
    return count;
}

static void write_bootcount(unsigned int count) {
    int fd = (int)sys_openat(AT_FDCWD, BOOTCOUNT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    sys_write(fd, &count, 4);
    sys_close(fd);
}

/* ── Status file ── */

static void write_status(const char *status) {
    int fd = (int)sys_openat(AT_FDCWD, STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    sys_write(fd, status, my_strlen(status));
    sys_close(fd);
}

/* ── Health signal (set by mods, checked by health thread) ── */

static volatile unsigned int *g_health_signal_ptr = 0;
#ifdef NATARTOS_BOOT_ACTIVATION
static int g_boot_health_mode = 0;
#endif

/* ── dlopen / dlsym / pthread via dynamic linker ── */

extern void *dlsym(void *handle, const char *symbol);

typedef void *(*dlopen_fn)(const char *, int);
typedef void *(*dlsym_fn)(void *, const char *);
typedef char *(*dlerror_fn)(void);
typedef int   (*pthread_create_fn)(void *, const void *, void *(*)(void *), void *);

static dlopen_fn   my_dlopen   = 0;
static dlerror_fn  my_dlerror  = 0;
static pthread_create_fn my_pthread_create = 0;

static int resolve_symbols(void) {
    my_dlopen = (dlopen_fn)dlsym(RTLD_DEFAULT, "dlopen");
    if (!my_dlopen) return -1;
    my_dlerror = (dlerror_fn)dlsym(RTLD_DEFAULT, "dlerror");
    my_pthread_create = (pthread_create_fn)dlsym(RTLD_DEFAULT, "pthread_create");
    if (!my_pthread_create) return -1;
    return 0;
}

/* ── Health thread ── */
#ifdef NATARTOS_BOOT_ACTIVATION
static void int_to_str(int v, char *out);
#endif

static void *health_thread_fn(void *arg) {
    (void)arg;
#ifdef NATARTOS_BOOT_ACTIVATION
    if (g_boot_health_mode) {
        struct timespec_t tick = { 1, 0 };
        for (int i = 0; i < HEALTH_DELAY_SEC; ++i) {
            sys_nanosleep(&tick, 0);
            if (g_health_signal_ptr && *g_health_signal_ptr > 0) {
                long sent=boot_health_finish(1);
                log_open();char report[16];int_to_str((int)sent,report);
                log_line2("boot health: report bytes=",report);
                if(sent==(long)sizeof(struct boot_health_message)) {
                    write_bootcount(0);write_status("healthy");
                } else write_status("error-boot-report");
                log_close();
                return 0;
            }
        }
        boot_health_finish(0);
        write_status("unhealthy-render");
        log_open();log_line("boot health: no frame signal");log_close();
        return 0;
    }
#endif
    struct timespec_t ts = { HEALTH_DELAY_SEC, 0 };
    sys_nanosleep(&ts, 0);

    log_open();
    /* Process survived 30s — no crash loop, reset bootcount.
     * Health signal is a bonus check, not a gate. */
    write_bootcount(0);
    if (g_health_signal_ptr && *g_health_signal_ptr > 0) {
        write_status("healthy");
        log_line("health: mod signal OK, counter reset");
    } else {
        write_status("healthy-alive");
        log_line("health: process alive 30s, counter reset");
    }
    log_close();
    return 0;
}

/* ── Mod loading ── */

#define MAX_MODS 16
#define PATH_BUF 256
#define DIRENT_BUF 2048

static void int_to_str(int v, char *out) {
    if (v < 0) { *out++ = '-'; v = -v; }
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[16];
    int i = 0;
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    out[i] = 0;
}

static void *g_mod_handles[MAX_MODS];
static int    g_mod_count = 0;

static int load_mods(void) {
    int dir_fd = (int)sys_openat(AT_FDCWD, MODS_DIR, O_RDONLY, 0);
    if (dir_fd < 0) {
        log_line("mods: dir not found");
        return 0;
    }
    static char buf[DIRENT_BUF] __attribute__((aligned(8)));

    for (;;) {
        long nread = sys_getdents64(dir_fd, buf, DIRENT_BUF);
        if (nread <= 0) break;

        long pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);

            if (ends_with_so(d->d_name)) {
                int dir_len = my_strlen(MODS_DIR);
                int fname_len = my_strlen(d->d_name);
                if (dir_len + 1 + fname_len >= PATH_BUF) {
                    pos += d->d_reclen;
                    continue;
                }
                char path[PATH_BUF];
                my_strcpy(path, MODS_DIR);
                path[dir_len] = '/';
                my_strcpy(path + dir_len + 1, d->d_name);

                /* Opt-in: load only if /vendor/natartos/enabled/<name> exists */
                int name_len = my_strlen(d->d_name);
                #define ENABLED_DIR "/vendor/natartos/enabled/"
                #define ENABLED_DIR_LEN 25
                if (ENABLED_DIR_LEN + name_len >= PATH_BUF) {
                    pos += d->d_reclen;
                    continue;
                }
                char on_path[PATH_BUF];
                my_strcpy(on_path, ENABLED_DIR);
                my_strcpy(on_path + ENABLED_DIR_LEN, d->d_name);
                log_line2("check: ", on_path);
                if (sys_faccessat(AT_FDCWD, on_path, 0) != 0) {
                    log_line2("skip: ", d->d_name);
                    pos += d->d_reclen;
                    continue;
                }

                log_line2("load: ", d->d_name);

                void *handle = my_dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
                if (handle) {
                    typedef int (*mod_init_fn)(void *(*)(void *, const char *));
                    mod_init_fn mod_init =
                        (mod_init_fn)dlsym(handle, "natartos_mod_init");
                    if (mod_init) {
                        log_line2("  init: ", d->d_name);
                        mod_init(dlsym);
                    }

                    if (!g_health_signal_ptr) {
                        volatile unsigned int *sig =
                            (volatile unsigned int *)dlsym(handle, "natartos_health_signal");
                        if (sig) {
                            g_health_signal_ptr = sig;
                            log_line("  health signal found");
                        }
                    }

                    if (g_mod_count < MAX_MODS)
                        g_mod_handles[g_mod_count++] = handle;
                } else {
                    /* dlopen failed — try RTLD_DEFAULT (LD_PRELOAD module) */
                    typedef int (*mod_init_fn)(void *(*)(void *, const char *));
                    mod_init_fn mod_init =
                        (mod_init_fn)dlsym(RTLD_DEFAULT, "natartos_mod_init");
                    if (mod_init) {
                        log_line2("  preload: ", d->d_name);
                        mod_init(dlsym);
                    } else {
                        char *err = my_dlerror ? my_dlerror() : 0;
                        log_line3("  FAIL: ", d->d_name, err ? err : "");
                    }
                }
            }

            pos += d->d_reclen;
        }
    }

    sys_close(dir_fd);
    return g_mod_count;
}

/* Phase 2: after all mods loaded, let them register menu items etc. */
static void register_mods(void) {
    typedef int (*mod_register_fn)(void *(*)(void *, const char *));
    for (int i = 0; i < g_mod_count; i++) {
        mod_register_fn reg =
            (mod_register_fn)dlsym(g_mod_handles[i], "natartos_mod_register");
        if (reg) {
            log_line("  calling mod_register");
            reg(dlsym);
        }
    }
}

/* ── Constructor (entry point) ── */

static int is_dji_glasses(void) {
    char buf[256];
    int fd = (int)sys_openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd < 0) return 0;
    long r = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (r <= 0) return 0;
    buf[r] = 0;
    for (int i = 0; i + 11 <= r; i++) {
        if (my_strcmp(buf + i, "dji_glasses") == 0) return 1;
        if (buf[i] == 0) break;
    }
    return 0;
}

__attribute__((constructor))
static void natartos_init(void) {
    if (!is_dji_glasses()) return;

    log_open();
    log_line("=== natartos loader v2 ===");

    /* 1. Read boot counter */
    unsigned int count = read_bootcount();

    /* 2. Check safe mode conditions */
    int safe_mode = 0;

    if (count >= SAFE_THRESHOLD) {
        safe_mode = 1;
        log_line("SAFE MODE: boot count >= threshold");
    }

    if (sys_faccessat(AT_FDCWD, DISABLED_PATH, 0) == 0) {
        safe_mode = 1;
        log_line("SAFE MODE: kill-switch active");
    }

    /* 3. Increment counter */
    write_bootcount(count + 1);

    /* 4. If safe mode, skip mods */
    if (safe_mode) {
        write_status("safe");
        log_line("mods skipped, running stock");
        log_close();
        return;
    }

    /* 5. Resolve dlopen/pthread */
    if (resolve_symbols() < 0) {
        log_line("FAIL: cannot resolve dlopen/pthread");
        write_status("error");
        log_close();
        return;
    }

#ifdef NATARTOS_BOOT_ACTIVATION
    typedef char *(*getenv_fn)(const char *);
    g_boot_health_mode = boot_health_setup((getenv_fn)dlsym(RTLD_DEFAULT, "getenv"));
    char health_mode[16];int_to_str(g_boot_health_mode,health_mode);
    log_line2("boot health: mode=",health_mode);
    if (g_boot_health_mode < 0) {
        write_status("error-boot-health");
        log_line("FAIL: invalid boot health channel");
        log_close();
        return;
    }
#endif

    /* 5b. Pre-load libdl.so globally so mods can resolve dlsym */
    my_dlopen("libdl.so", RTLD_LAZY | RTLD_GLOBAL);

    /* 6. Load mods (phase 1: init) */
    int n = load_mods();
    char nbuf[16];
    int_to_str(n, nbuf);
    log_line2("mods loaded: ", nbuf);

    /* 7. Register phase (menu items etc. — all symbols now available) */
    register_mods();

    write_status("active");

    /* 8. Start health thread */
    unsigned long thr = 0;
    if (my_pthread_create((void *)&thr, 0, health_thread_fn, 0) == 0) {
        log_line("health thread started");
    } else {
        log_line("health thread FAILED");
    }

    log_close();
}
