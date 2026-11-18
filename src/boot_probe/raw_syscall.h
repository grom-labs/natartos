/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef NATARTOS_BOOT_RAW_SYSCALL_H
#define NATARTOS_BOOT_RAW_SYSCALL_H

#if defined(__aarch64__)
enum { NR_openat = 56, NR_close = 57, NR_read = 63, NR_write = 64,
       NR_flock = 32, NR_getpid = 172, NR_getuid = 174, NR_geteuid = 175,
       NR_getgid = 176, NR_getegid = 177, NR_statx = 291, NR_exit_group = 94,
       NR_fcntl = 25, NR_clone = 220, NR_execve = 221, NR_wait4 = 260,
       NR_setsid = 157, NR_dup3 = 24, RAW_O_NOFOLLOW = 0100000 };
static inline long raw_call(long n, long a, long b, long c, long d, long e) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e, x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}
#elif defined(__x86_64__)
enum { NR_openat = 257, NR_close = 3, NR_read = 0, NR_write = 1,
       NR_flock = 73, NR_getpid = 39, NR_getuid = 102, NR_geteuid = 107,
       NR_getgid = 104, NR_getegid = 108, NR_statx = 332, NR_exit_group = 231,
       NR_fcntl = 72, NR_clone = 56, NR_execve = 59, NR_wait4 = 61,
       NR_setsid = 112, NR_dup3 = 292, RAW_O_NOFOLLOW = 0400000 };
static inline long raw_call(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e;
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b),
                     "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory", "cc");
    return result;
}
#else
#error Boot probe supports Linux AArch64 and x86-64 only.
#endif

static inline long raw_open(const char *path, long flags) {
    return raw_call(NR_openat, -100, (long)path, flags, 0, 0);
}
static inline void raw_close(long fd) { (void)raw_call(NR_close, fd, 0, 0, 0, 0); }

#endif
