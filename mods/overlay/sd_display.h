/* Shared by the real overlay and the host overlay fixture. Callers provide
 * sys_openat/close/flock/pread64/pwrite64, returning raw negative errno values.
 * No libc dependency: the device build uses ARM64 syscalls.
 */
#ifndef NATARTOS_SD_DISPLAY_H
#define NATARTOS_SD_DISPLAY_H
#include "natartos_display.h"

#define SD_LOCK_EX 2
#define SD_LOCK_NB 4
#define SD_LOCK_UN 8
#define SD_LOCK_BUSY (-11) /* EAGAIN/EWOULDBLOCK on Linux */
#define SD_OPEN_LOCK (2 | 0100 | 0400000 | 02000000) /* RDWR|CREAT|NOFOLLOW|CLOEXEC */

typedef struct {
    int owner, lease, managed;
} nat_overlay_session;

/* The runner holds an exclusive owner lease. Probe without disturbing it. */
static int nat_overlay_owner_alive(nat_overlay_session *s) {
    if (!s->managed) return 1;
    long rc = sys_flock(s->owner, SD_LOCK_EX | SD_LOCK_NB);
    if (rc == SD_LOCK_BUSY) return 1;
    if (!rc) sys_flock(s->owner, SD_LOCK_UN);
    return 0;
}

static void nat_overlay_detach(nat_overlay_session *s) {
    if (s->owner >= 0) sys_close(s->owner);
    if (s->lease >= 0) sys_close(s->lease);
    s->owner = s->lease = -1;
    s->managed = 0;
}

/* Keep the overlay lease until repaint/cleanup finishes, including for legacy
 * apps, so a new SD session cannot race a previous overlay's cleanup. */
static int nat_overlay_attach(nat_overlay_session *s, const char *frame_path,
                              const char *owner_path, const char *overlay_path) {
    int frame = -1;
    s->owner = s->lease = -1;
    s->managed = 0;
    s->lease = (int)sys_openat(-100, overlay_path, SD_OPEN_LOCK, 0600);
    if (s->lease < 0 || sys_flock(s->lease, SD_LOCK_EX | SD_LOCK_NB)) goto bad;
    /* Open under the lease: a pre-opened old inode could otherwise be attached
     * after the runner replaces a dead session's frame file. */
    frame = (int)sys_openat(-100, frame_path, 2 | 0400000 | 02000000, 0);
    if (frame < 0) goto bad;
    char magic[NAT_DISPLAY_MAGIC_BYTES];
    if (sys_pread64(frame, magic, sizeof(magic), NAT_DISPLAY_MAGIC_OFFSET) == (long)sizeof(magic)) {
        s->managed = 1;
        for (unsigned i = 0; i < sizeof(magic); i++)
            if (magic[i] != NAT_DISPLAY_MAGIC[i]) s->managed = 0;
    }
    if (s->managed) {
        s->owner = (int)sys_openat(-100, owner_path, SD_OPEN_LOCK, 0600);
        if (s->owner < 0 || !nat_overlay_owner_alive(s)) goto bad;
    }
    return frame;
bad:
    if (frame >= 0) sys_close(frame);
    nat_overlay_detach(s);
    return -1;
}

/* The frame inode itself identifies the session; an old acknowledgement in
 * an unlinked file cannot acknowledge the next runner's newly-created file. */
static int nat_overlay_ack(nat_overlay_session *s, int frame, unsigned ready) {
    if (!s->managed) return 0;
    return sys_pwrite64(frame, &ready, sizeof(ready), NAT_DISPLAY_ACK_OFFSET) == (long)sizeof(ready) ? 0 : -1;
}

#endif
