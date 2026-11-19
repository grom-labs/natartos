/*
 * Host stand-in for include/natartos_common.h, used only to compile
 * mods/sd_apps/mod.c into tests/sd_apps/mod_check.c. The real header uses
 * ARM64 syscall instructions; this one maps the same helpers onto libc.
 * Keep the menu API below identical to include/natartos_common.h.
 */
#ifndef NATARTOS_COMMON_H
#define NATARTOS_COMMON_H

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RTLD_DEFAULT ((void *)0)
#ifndef NATARTOS_LOG_PATH
#define NATARTOS_LOG_PATH "/dev/null"
#endif

static inline long sys_openat(int dirfd, const char *path, int flags, int mode) { return openat(dirfd, path, flags, mode); }
static inline long sys_read(int fd, void *buf, long count) { return read(fd, buf, (size_t)count); }
static inline long sys_close(int fd) { return close(fd); }
static inline long sys_faccessat(int dirfd, const char *path, int mode) { return faccessat(dirfd, path, mode, 0); }
static inline int natartos_strlen(const char *s) { return (int)strlen(s); }
static inline int natartos_strncmp(const char *a, const char *b, int n) { return strncmp(a, b, (size_t)n); }
static inline void natartos_memcpy(void *dst, const void *src, long n) { memcpy(dst, src, (size_t)n); }

#ifdef NTLOG_PREFIX
#define NTLOG(msg)          fprintf(stderr, "[%s] %s\n", NTLOG_PREFIX, (msg))
#define NTLOG_INT(label, v) fprintf(stderr, "[%s] %s%d\n", NTLOG_PREFIX, (label), (v))
#endif

/* --- Menu registration API (same as include/natartos_common.h) --- */

typedef struct {
    const char *id;
    const char *name;
    const char *subtitle;
    const char *icon_path;
    const char *command;
    void (*on_action)(void);
    int priority;
} natartos_menu_entry;

#define NATARTOS_MAX_MENU_ITEMS 32

typedef int (*natartos_menu_register_fn)(const natartos_menu_entry *entry);

#endif /* NATARTOS_COMMON_H */
