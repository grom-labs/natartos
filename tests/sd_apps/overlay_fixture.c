/* Exercise the actual overlay lease/acknowledgement code with real Linux file
 * locks. No display hardware is emulated; the fixture supplies frame readiness
 * and a delayed repaint, then uses the same lifecycle helper as hook_overlay.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static long sys_openat(int dir, const char *path, int flags, int mode) {
    int fd = openat(dir, path, flags, mode);
    return fd < 0 ? -errno : fd;
}
static long sys_close(int fd) { return close(fd); }
static long sys_flock(int fd, int op) { return flock(fd, op) ? -errno : 0; }
static long sys_pread64(int fd, void *buf, long n, long offset) { return pread(fd, buf, (size_t)n, offset); }
static long sys_pwrite64(int fd, const void *buf, long n, long offset) { return pwrite(fd, buf, (size_t)n, offset); }
#include "../../mods/overlay/sd_display.h"

static volatile sig_atomic_t stop;
static void on_stop(int sig) { (void)sig; stop = 1; }
static void pause_ms(long ms) {
    struct timespec t = {ms / 1000, ms % 1000 * 1000000};
    nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char frame[PATH_MAX], flag[PATH_MAX], request[PATH_MAX], owner[PATH_MAX], lease[PATH_MAX], cleaning[PATH_MAX], attached[PATH_MAX];
    snprintf(frame, sizeof(frame), "%s/fb_shared", argv[1]);
    snprintf(flag, sizeof(flag), "%s/fb_override", argv[1]);
    snprintf(request, sizeof(request), "%s/fb_stop", argv[1]);
    snprintf(owner, sizeof(owner), "%s/" NAT_DISPLAY_OWNER, argv[1]);
    snprintf(lease, sizeof(lease), "%s/" NAT_DISPLAY_OVERLAY, argv[1]);
    snprintf(cleaning, sizeof(cleaning), "%s/cleaning", argv[1]);
    snprintf(attached, sizeof(attached), "%s/attached", argv[1]);
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    int no_frames = argc > 2 && atoi(argv[2]);
    while (!stop) {
        if (access(flag, F_OK)) { pause_ms(10); continue; }
        nat_overlay_session s;
        int fd = nat_overlay_attach(&s, frame, owner, lease);
        if (fd < 0) { pause_ms(10); continue; }
        int mark = open(attached, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (mark >= 0) close(mark);
        if (!no_frames && nat_overlay_ack(&s, fd, NAT_DISPLAY_ACK_READY)) return 3;
        while (!stop && !access(flag, F_OK) && access(request, F_OK) && nat_overlay_owner_alive(&s))
            pause_ms(10);
        nat_overlay_ack(&s, fd, 0);
        close(fd);
        unlink(flag); unlink(request); unlink(frame);
        unlink(attached);
        int marker = open(cleaning, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (marker >= 0) close(marker);
        pause_ms(200);  /* the overlay lease must outlast repaint */
        unlink(cleaning);
        nat_overlay_detach(&s);
    }
    return 0;
}
