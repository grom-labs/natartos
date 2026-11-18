/* SPDX-License-Identifier: AGPL-3.0-only
 * Optional freestanding fixture for a user-supplied Android ARM64 linker/libdl.
 * Runs the unmodified production proxy against our authored stock fixture.
 */
#include <stddef.h>
#include <stdint.h>
#include "raw_syscall.h"
#if !defined(__aarch64__)
#error Android fixture is ARM64 only.
#endif
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
typedef const void *(*getter_fn)(void *);
struct interface { unsigned long size; int (*init)(void *); int (*start)(void); };
__attribute__((noreturn)) static void finish(int result) {
    const char *message = result ? "ANDROID_BOOT_PROBE_SMOKE=FAIL\n" : "ANDROID_BOOT_PROBE_SMOKE=PASS\n";
    size_t size = 0; while (message[size]) ++size;
    (void)raw_call(NR_write, 1, (long)message, (long)size, 0, 0);
    (void)raw_call(NR_exit_group, result, 0, 0, 0, 0);
    for (;;) {}
}
static int contains(const char *data, long length, const char *needle) {
    for (long i = 0; i < length; ++i) {
        long j = 0;
        while (needle[j] && i + j < length && data[i + j] == needle[j]) ++j;
        if (!needle[j]) return 1;
    }
    return 0;
}
__attribute__((noreturn)) void _start(void) {
    void *proxy = dlopen("/vendor/lib64/gps.default.so", 2);
    if (!proxy) {
        const char *error = dlerror();
        if (error) { size_t n = 0; while (error[n]) ++n; (void)raw_call(NR_write, 1, (long)error, (long)n, 0, 0); }
        finish(1);
    }
    getter_fn getter = (getter_fn)dlsym(proxy, "gps_get_hardware_interface");
    if (!getter) finish(2);
    void *stock = dlopen("/vendor/lib64/gps.default.stock.so", 2);
    if (!stock) finish(3);
    getter_fn original = (getter_fn)dlsym(stock, "gps_get_hardware_interface");
    if (!original) finish(4);
    const struct interface *table = getter((void *)(uintptr_t)0x1234);
    if (!table || table != original((void *)(uintptr_t)0x1234) ||
        table->init((void *)(uintptr_t)0x1234) != 17 || table->start() != 23 ||
        getter((void *)(uintptr_t)0x2345) != original((void *)(uintptr_t)0x2345) ||
        getter((void *)(uintptr_t)0xDEAD) != NULL) finish(5);
    long fd = raw_open("/data/dji/natartos_boot_probe.log", 0);
    if (fd < 0) finish(6);
    char data[1536];
    long length = raw_call(NR_read, fd, (long)data, sizeof(data), 0, 0);
    raw_close(fd);
    if (!contains(data, length, "natartos_boot_probe v=1") ||
        !contains(data, length, " stock=ready ") || !contains(data, length, " CapEff=")) finish(7);
    finish(0);
}
