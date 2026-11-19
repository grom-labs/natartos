/*
 * natartos_app.h — helpers for standalone apps that draw on the goggles.
 *
 * Header-only, for static executables built against an ordinary libc (musl
 * or glibc). Mods loaded into dji_glasses use natartos_common.h instead.
 *
 * Frame buffer
 * ------------
 * hook_overlay.so copies frames from a shared file onto the display while the
 * overlay flag (/data/local/tmp/fb_override) exists. The layout matches what
 * the overlay's inject thread reads and what fb_receiver writes:
 *
 *   offset 0                 u32 frame counter
 *   offsets 8-19             launcher/overlay handshake; apps must leave unchanged
 *   rest of 64-byte header   reserved
 *   offset 64                slot 0: 1920x1080 pixels, bytes B,G,R,A
 *   offset 64 + slot bytes   slot 1
 *
 * The overlay shows slot (counter & 1). Draw into the other slot, then bump
 * the counter (nat_fb_publish), so a frame is never shown half-drawn.
 * Never resize, truncate or zero the file: the launcher owns it.
 *
 * SD apps get the file path in NATARTOS_FB_PATH and the button devices in
 * NATARTOS_INPUT_DEVICES (colon-separated evdev paths); see
 * docs/SD_APP_RUNNER.md.
 */
#ifndef NATARTOS_APP_H
#define NATARTOS_APP_H

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAT_FB_WIDTH 1920
#define NAT_FB_HEIGHT 1080
#define NAT_FB_HEADER 64
#define NAT_FB_SLOT_BYTES ((size_t)NAT_FB_WIDTH * NAT_FB_HEIGHT * 4)
#define NAT_FB_FILE_BYTES ((size_t)NAT_FB_HEADER + 2 * NAT_FB_SLOT_BYTES)

/* Buttons. The 5D centre press reports KEY_ENTER on gpio_keys; BACK is KEY_BACK. */
#define NAT_KEY_PRESS KEY_ENTER
#define NAT_KEY_BACK KEY_BACK
#define NAT_INPUT_DEFAULT "/dev/input/event0:/dev/input/event3"
#define NAT_INPUT_MAX 4

/* ---- Frame buffer ---- */

typedef struct {
    unsigned char *map;
    uint32_t *pixels;   /* slot to draw next: NAT_FB_WIDTH*NAT_FB_HEIGHT, 0xAARRGGBB */
} nat_fb;

static inline uint32_t *nat_fb_begin(nat_fb *fb) {
    uint32_t count = __atomic_load_n((uint32_t *)fb->map, __ATOMIC_ACQUIRE);
    fb->pixels = (uint32_t *)(fb->map + NAT_FB_HEADER + ((count + 1u) & 1u) * NAT_FB_SLOT_BYTES);
    return fb->pixels;
}

/* Map the frame file the launcher created. 0, or -1 with errno set. */
static inline int nat_fb_open(nat_fb *fb, const char *path) {
    fb->map = NULL;
    fb->pixels = NULL;
    if (!path || !*path) { errno = EINVAL; return -1; }
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != NAT_FB_FILE_BYTES) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    void *map = mmap(NULL, NAT_FB_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (map == MAP_FAILED) { errno = saved; return -1; }
    fb->map = (unsigned char *)map;
    nat_fb_begin(fb);
    return 0;
}

/* Show the slot just drawn; fb->pixels then points at the next slot to draw. */
static inline void nat_fb_publish(nat_fb *fb) {
    __atomic_add_fetch((uint32_t *)fb->map, 1u, __ATOMIC_RELEASE);
    nat_fb_begin(fb);
}

static inline void nat_fb_close(nat_fb *fb) {
    if (fb->map) munmap(fb->map, NAT_FB_FILE_BYTES);
    fb->map = NULL;
    fb->pixels = NULL;
}

/* ---- Drawing ---- */

static inline uint32_t nat_rgb(unsigned r, unsigned g, unsigned b) {
    return 0xff000000u | (r & 255u) << 16 | (g & 255u) << 8 | (b & 255u);
}

static inline void nat_fill(uint32_t *px, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > NAT_FB_WIDTH) w = NAT_FB_WIDTH - x;
    if (y + h > NAT_FB_HEIGHT) h = NAT_FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; ++row) {
        uint32_t *p = px + (size_t)row * NAT_FB_WIDTH + x;
        for (int i = 0; i < w; ++i) p[i] = color;
    }
}

static inline void nat_frame(uint32_t *px, int x, int y, int w, int h, int t, uint32_t color) {
    nat_fill(px, x, y, w, t, color);
    nat_fill(px, x, y + h - t, w, t, color);
    nat_fill(px, x, y, t, h, color);
    nat_fill(px, x + w - t, y, t, h, color);
}

#define NAT_GLYPH_W 5
#define NAT_GLYPH_H 7
#define NAT_GLYPH_ADVANCE 6

/* 5x7 glyphs for ASCII 0x20-0x7e, one byte per row, bit 4 = leftmost column. */
static inline const unsigned char *nat_glyph(unsigned char c) {
    static const unsigned char font[95][NAT_GLYPH_H] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
        {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
        {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /* " */
        {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* # */
        {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
        {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
        {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* & */
        {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
        {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
        {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
        {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
        {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
        {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, /* , */
        {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
        {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* / */
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
        {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
        {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, /* ; */
        {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
        {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
        {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
        {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
        {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
        {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /* A */
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
        {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
        {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
        {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
        {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* \ */
        {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
        {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
        {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
        {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /* ` */
        {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* a */
        {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, /* b */
        {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* c */
        {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, /* d */
        {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* e */
        {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* f */
        {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, /* g */
        {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, /* h */
        {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* i */
        {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* j */
        {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* k */
        {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* l */
        {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* m */
        {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, /* n */
        {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* o */
        {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* p */
        {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, /* q */
        {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* r */
        {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, /* s */
        {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* t */
        {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* u */
        {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* v */
        {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* w */
        {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* x */
        {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* y */
        {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* z */
        {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* { */
        {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* | */
        {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* } */
        {0x00,0x00,0x00,0x0D,0x12,0x00,0x00}, /* ~ */
    };
    if (c < 0x20 || c > 0x7e) c = '?';
    return font[c - 0x20];
}

/* Bytes that start a displayed character: ASCII, or the lead byte of a UTF-8
 * sequence (drawn as '?'). Continuation bytes take no space. */
static inline int nat_text_chars(const char *text) {
    int n = 0;
    for (const unsigned char *s = (const unsigned char *)text; *s; ++s)
        if ((*s & 0xc0) != 0x80) ++n;
    return n;
}

static inline int nat_text_width(const char *text, int scale) {
    int n = nat_text_chars(text);
    return n ? (n * NAT_GLYPH_ADVANCE - 1) * scale : 0;
}

/* Draw UTF-8 text in the 5x7 font; non-ASCII characters show as '?'.
 * Returns the drawn width in pixels. */
static inline int nat_text(uint32_t *px, int x, int y, int scale, uint32_t color, const char *text) {
    int start = x;
    for (const unsigned char *s = (const unsigned char *)text; *s; ++s) {
        if ((*s & 0xc0) == 0x80) continue;
        const unsigned char *rows = nat_glyph(*s < 0x80 ? *s : '?');
        for (int r = 0; r < NAT_GLYPH_H; ++r)
            for (int c = 0; c < NAT_GLYPH_W; ++c)
                if (rows[r] & (0x10 >> c))
                    nat_fill(px, x + c * scale, y + r * scale, scale, scale, color);
        x += NAT_GLYPH_ADVANCE * scale;
    }
    return x == start ? 0 : x - start - scale;
}

static inline int nat_text_centered(uint32_t *px, int y, int scale, uint32_t color, const char *text) {
    return nat_text(px, (NAT_FB_WIDTH - nat_text_width(text, scale)) / 2, y, scale, color, text);
}

/* ---- Buttons ---- */

typedef struct {
    int fds[NAT_INPUT_MAX];
    unsigned count;
} nat_input;

/* Open the goggles' button devices for exclusive use, so the DJI interface
 * underneath does not also react. `devices` is a colon-separated list (NULL:
 * NAT_INPUT_DEFAULT). Returns the number opened. */
static inline unsigned nat_input_open(nat_input *in, const char *devices) {
    char path[256];
    in->count = 0;
    if (!devices || !*devices) devices = NAT_INPUT_DEFAULT;
    while (*devices && in->count < NAT_INPUT_MAX) {
        const char *end = strchr(devices, ':');
        size_t length = end ? (size_t)(end - devices) : strlen(devices);
        if (length && length < sizeof(path)) {
            memcpy(path, devices, length);
            path[length] = 0;
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                /* A grab can fail (e.g. a test FIFO); the device still works. */
                (void)ioctl(fd, EVIOCGRAB, 1);
                in->fds[in->count++] = fd;
            }
        }
        if (!end) break;
        devices = end + 1;
    }
    return in->count;
}

static inline void nat_input_close(nat_input *in) {
    for (unsigned i = 0; i < in->count; ++i) close(in->fds[i]);
    in->count = 0;
}

/* Wait up to timeout_ms (-1 = forever) for one key event (EV_KEY only).
 * 1: *ev filled. 0: timeout or signal. -1: every device is gone. */
static inline int nat_input_next(nat_input *in, struct input_event *ev, int timeout_ms) {
    for (;;) {
        for (unsigned i = 0; i < in->count; ++i) {
            for (;;) {
                ssize_t n = read(in->fds[i], ev, sizeof(*ev));
                if (n == (ssize_t)sizeof(*ev)) {
                    if (ev->type == EV_KEY) return 1;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                /* EOF, short read or ENODEV: drop this device. */
                close(in->fds[i]);
                in->fds[i--] = in->fds[--in->count];
                break;
            }
        }
        if (!in->count) return -1;
        if (timeout_ms == 0) return 0;
        struct pollfd p[NAT_INPUT_MAX];
        for (unsigned i = 0; i < in->count; ++i) p[i] = (struct pollfd){in->fds[i], POLLIN, 0};
        int ready = poll(p, in->count, timeout_ms);
        if (ready <= 0) return 0;
        timeout_ms = 0;   /* read what arrived, then report a timeout if it was not a key */
    }
}

/* Joystick direction as the wearer sees it: 0 none, else 'U','D','L','R'.
 * The 5D reports its directions rotated 180 degrees; this mirrors the mapping
 * in the tested Doom port (mods/doom/doomgeneric_goggles.c). */
static inline int nat_joystick(unsigned code) {
    switch (code) {
    case KEY_DOWN: return 'U';
    case KEY_UP: return 'D';
    case KEY_RIGHT: return 'L';
    case KEY_LEFT: return 'R';
    default: return 0;
    }
}

#endif /* NATARTOS_APP_H */
