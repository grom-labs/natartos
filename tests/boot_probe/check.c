/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef const void *(*getter_fn)(void *);
struct interface { unsigned long size; int (*init)(void *); int (*start)(void); };
static char *proxy_path, *stock_path, *missing_path;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); return 1; } } while (0)

static int make_marker(void) {
    int fd = open("marker", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
    return fd >= 0;
}
static int load_forward(int expect_null) {
    void *proxy = dlopen(proxy_path, RTLD_NOW | RTLD_LOCAL);
    if (!proxy) fprintf(stderr, "dlopen: %s\n", dlerror());
    REQUIRE(proxy);
    getter_fn getter = (getter_fn)dlsym(proxy, "gps_get_hardware_interface");
    REQUIRE(getter);
    if (expect_null) { REQUIRE(!getter((void *)(uintptr_t)0x1234)); return 0; }
    void *stock = dlopen("./stock.so", RTLD_NOW | RTLD_LOCAL);
    REQUIRE(stock);
    getter_fn original = (getter_fn)dlsym(stock, "gps_get_hardware_interface");
    REQUIRE(original);
    const struct interface *table = getter((void *)(uintptr_t)0x1234);
    REQUIRE(table == original((void *)(uintptr_t)0x1234));
    REQUIRE(getter((void *)(uintptr_t)0x2345) == original((void *)(uintptr_t)0x2345));
    REQUIRE(getter((void *)(uintptr_t)0x2345) != table);
    REQUIRE(getter((void *)(uintptr_t)0xDEAD) == NULL);
    REQUIRE(table->init((void *)(uintptr_t)0x1234) == 17 && table->start() == 23);
    REQUIRE(dlclose(stock) == 0 && dlclose(proxy) == 0);
    /* Proxy intentionally retains the original; returned callbacks still work. */
    REQUIRE(table->start() == 23);
    return 0;
}
static int record_contains(const char *needle) {
    char data[4096] = {0};
    int fd = open("marker", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t count = read(fd, data, sizeof(data) - 1);
    close(fd);
    return count >= 0 && strstr(data, needle) != NULL;
}
static off_t marker_size(void) {
    struct stat st;
    return stat("marker", &st) ? -1 : st.st_size;
}
static int basic(void) {
    REQUIRE(make_marker());
    REQUIRE(load_forward(0) == 0);
    REQUIRE(record_contains("natartos_boot_probe v=1 phase=load pid="));
    REQUIRE(record_contains(" stock=ready context="));
    REQUIRE(record_contains(" CapEff="));
    REQUIRE(record_contains(" enforcing=") && record_contains(" boot_id="));
    REQUIRE(marker_size() > 0 && marker_size() < 1536);
    return 0;
}
static int absent(void) { REQUIRE(load_forward(0) == 0); REQUIRE(access("marker", F_OK) < 0 && errno == ENOENT); return 0; }
static int directory_marker(void) { REQUIRE(mkdir("marker", 0700) == 0); REQUIRE(load_forward(0) == 0); return 0; }
static int fifo_marker(void) { REQUIRE(mkfifo("marker", 0600) == 0); REQUIRE(load_forward(0) == 0); REQUIRE(marker_size() == 0); return 0; }
static int symlink_marker(void) {
    int fd = open("target", O_CREAT | O_WRONLY, 0600); REQUIRE(fd >= 0); close(fd);
    REQUIRE(symlink("target", "marker") == 0); REQUIRE(load_forward(0) == 0);
    struct stat st; REQUIRE(stat("target", &st) == 0 && st.st_size == 0); return 0;
}
static int full_marker(void) {
    REQUIRE(make_marker()); REQUIRE(truncate("marker", 65536) == 0);
    REQUIRE(load_forward(0) == 0 && marker_size() == 65536); return 0;
}
static int near_full_marker(void) {
    REQUIRE(make_marker()); REQUIRE(truncate("marker", 65530) == 0);
    REQUIRE(load_forward(0) == 0 && marker_size() == 65530); return 0;
}
static int writable_marker(void) {
    REQUIRE(make_marker()); REQUIRE(chmod("marker", 0666) == 0);
    REQUIRE(load_forward(0) == 0 && marker_size() == 0); return 0;
}
static int locked_marker(void) {
    REQUIRE(make_marker()); int fd = open("marker", O_WRONLY); REQUIRE(fd >= 0);
    REQUIRE(flock(fd, LOCK_EX | LOCK_NB) == 0);
    REQUIRE(load_forward(0) == 0 && marker_size() == 0); close(fd); return 0;
}
static int append_marker(void) {
    REQUIRE(make_marker()); int fd = open("marker", O_WRONLY); REQUIRE(fd >= 0);
    REQUIRE(write(fd, "preserve\n", 9) == 9); close(fd); REQUIRE(load_forward(0) == 0);
    REQUIRE(record_contains("preserve\nnatartos_boot_probe")); return 0;
}
static int no_stock(void) {
    REQUIRE(unlink("stock.so") == 0 && make_marker()); REQUIRE(load_forward(1) == 0);
    REQUIRE(record_contains(" stock=open-failed ")); return 0;
}
static int no_symbol(void) {
    REQUIRE(unlink("stock.so") == 0 && symlink(missing_path, "stock.so") == 0 && make_marker());
    REQUIRE(load_forward(1) == 0 && record_contains(" stock=symbol-missing ")); return 0;
}
static int self_reference(void) {
    REQUIRE(unlink("stock.so") == 0 && symlink(proxy_path, "stock.so") == 0 && make_marker());
    REQUIRE(load_forward(1) == 0 && record_contains(" stock=self-reference ")); return 0;
}
struct thread_data { getter_fn getter; const void *expected; int failed; };
static void *call_thread(void *argument) {
    struct thread_data *t = argument;
    for (int i = 0; i < 10000; ++i) if (t->getter((void *)(uintptr_t)0x2345) != t->expected) t->failed = 1;
    return NULL;
}
static int concurrent(void) {
    REQUIRE(make_marker());
    void *one = dlopen(proxy_path, RTLD_NOW | RTLD_LOCAL); REQUIRE(one);
    off_t size = marker_size(); REQUIRE(size > 0);
    void *two = dlopen(proxy_path, RTLD_NOW | RTLD_LOCAL); REQUIRE(two == one && marker_size() == size);
    getter_fn getter = (getter_fn)dlsym(one, "gps_get_hardware_interface"); REQUIRE(getter);
    pthread_t threads[8]; struct thread_data data[8];
    for (int i = 0; i < 8; ++i) {
        data[i] = (struct thread_data){getter, getter((void *)(uintptr_t)0x2345), 0};
        REQUIRE(pthread_create(&threads[i], NULL, call_thread, &data[i]) == 0);
    }
    for (int i = 0; i < 8; ++i) { REQUIRE(pthread_join(threads[i], NULL) == 0 && !data[i].failed); }
    REQUIRE(marker_size() == size); return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: check PROXY STOCK MISSING_SYMBOL\n"); return 2; }
    proxy_path = realpath(argv[1], NULL); stock_path = realpath(argv[2], NULL); missing_path = realpath(argv[3], NULL);
    REQUIRE(proxy_path && stock_path && missing_path);
    struct test { const char *name; int (*run)(void); } tests[] = {
        {"forwarding and diagnostics", basic}, {"missing marker", absent},
        {"directory marker", directory_marker}, {"FIFO marker", fifo_marker},
        {"symlink marker", symlink_marker}, {"full marker", full_marker},
        {"near-full marker", near_full_marker}, {"writable marker", writable_marker},
        {"locked marker", locked_marker}, {"preserve existing content", append_marker},
        {"missing original", no_stock}, {"missing getter", no_symbol},
        {"self-reference", self_reference}, {"concurrent forwarding and single record", concurrent},
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        fflush(NULL);
        pid_t pid = fork(); REQUIRE(pid >= 0);
        if (!pid) {
            alarm(5);
            char temporary[] = "/tmp/natartos-boot-probe.XXXXXX";
            if (!mkdtemp(temporary) || chdir(temporary) || symlink(stock_path, "stock.so")) _exit(2);
            int result = tests[i].run();
            unlink("stock.so"); unlink("marker"); rmdir("marker"); unlink("target");
            if (chdir("/") || rmdir(temporary)) result = 1;
            _exit(result);
        }
        int status; REQUIRE(waitpid(pid, &status, 0) == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "FAIL %s status=%d\n", tests[i].name, status); return 1; }
        printf("PASS %s\n", tests[i].name);
    }
    printf("BOOT_PROBE_TESTS_PASSED=%zu\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
