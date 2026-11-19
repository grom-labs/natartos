/*
 * Hello Goggles — the SD Apps example.
 *
 * Launched as a display app ("display": true), it draws through the natartos
 * overlay and reads the goggles' buttons: the joystick moves a highlight,
 * the 5D press changes its colour, and BACK exits. Without a display (the
 * runner's console use and the host tests) it prints its message and exits.
 */
#define _POSIX_C_SOURCE 200809L
#include "natartos_app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID_COLS 7
#define GRID_ROWS 3
#define TILE 120
#define GAP 20

static volatile sig_atomic_t stop_requested;
static void on_stop(int sig) { (void)sig; stop_requested = 1; }

typedef struct {
    char message[256];
    const char *source;   /* package folder the runner staged from */
    unsigned launches, presses;
    int col, row, colour;
} hello_state;

/* The runner says where the package came from; the SD card is the external
 * FAT/exFAT volume under /mnt/media_rw (not the internal "emulated" one). */
static const char *source_place(const char *source) {
    if (!source || !*source) return "Loaded from an unknown location";
    if (!strncmp(source, "/mnt/media_rw/", 14) && strncmp(source, "/mnt/media_rw/emulated", 22))
        return "Loaded from the SD card";
    return "Loaded from internal storage";
}

static const unsigned palette[][3] = {
    {0x2d, 0x8c, 0xff}, {0x2e, 0xcc, 0x71}, {0xf1, 0xc4, 0x0f}, {0xe7, 0x4c, 0x3c}, {0x9b, 0x59, 0xb6},
};
#define PALETTE_SIZE (int)(sizeof(palette) / sizeof(palette[0]))

static void draw(nat_fb *fb, const hello_state *s) {
    uint32_t *px = fb->pixels;
    nat_fill(px, 0, 0, NAT_FB_WIDTH, NAT_FB_HEIGHT, nat_rgb(0x12, 0x14, 0x1a));
    nat_text_centered(px, 120, 12, nat_rgb(255, 255, 255), "Hello Goggles");
    nat_text_centered(px, 250, 4, nat_rgb(0xb0, 0xb8, 0xc8), s->message);
    nat_text_centered(px, 300, 4, nat_rgb(0x2e, 0xcc, 0x71), source_place(s->source));
    if (s->source) nat_text_centered(px, 345, 2, nat_rgb(0x80, 0x88, 0x98), s->source);

    const unsigned *c = palette[s->colour];
    int grid_w = GRID_COLS * (TILE + GAP) - GAP, grid_h = GRID_ROWS * (TILE + GAP) - GAP;
    int x0 = (NAT_FB_WIDTH - grid_w) / 2, y0 = 400;
    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int k = 0; k < GRID_COLS; ++k) {
            int x = x0 + k * (TILE + GAP), y = y0 + r * (TILE + GAP);
            if (r == s->row && k == s->col) nat_fill(px, x, y, TILE, TILE, nat_rgb(c[0], c[1], c[2]));
            else nat_frame(px, x, y, TILE, TILE, 4, nat_rgb(0x3a, 0x40, 0x4c));
        }
    }

    char status[96];
    snprintf(status, sizeof(status), "Launch #%u    Presses: %u", s->launches, s->presses);
    nat_text_centered(px, y0 + grid_h + 40, 4, nat_rgb(0xe0, 0xe4, 0xec), status);
    nat_text_centered(px, 940, 3, nat_rgb(0x80, 0x88, 0x98),
                      "Joystick: move    Press: colour    Back: exit");
    nat_fb_publish(fb);
}

/* Returns 0 on a normal exit (BACK, SIGTERM or the buttons going away). */
static int run_display(hello_state *s) {
    struct sigaction sa = {0};
    sa.sa_handler = on_stop;   /* no SA_RESTART: poll() returns at once */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    nat_fb fb;
    if (nat_fb_open(&fb, getenv("NATARTOS_FB_PATH"))) { perror("frame buffer"); return 1; }
    nat_input in;
    if (!nat_input_open(&in, getenv("NATARTOS_INPUT_DEVICES"))) {
        fputs("no button devices could be opened\n", stderr);
        nat_fb_close(&fb);
        return 1;
    }
    int back_down = 0, dirty = 1;
    while (!stop_requested) {
        if (dirty) { draw(&fb, s); dirty = 0; }
        struct input_event ev;
        int got = nat_input_next(&in, &ev, 500);
        if (got < 0) { fputs("button devices closed\n", stderr); break; }
        if (!got) continue;
        if (ev.code == NAT_KEY_BACK) {
            /* Leave on release, so the release does not reach the DJI UI. */
            if (ev.value == 1) back_down = 1;
            else if (ev.value == 0 && back_down) break;
            continue;
        }
        if (ev.value == 0) continue;   /* act on press and auto-repeat */
        if (ev.code == NAT_KEY_PRESS && ev.value == 1) {
            s->colour = (s->colour + 1) % PALETTE_SIZE;
            ++s->presses;
            dirty = 1;
            continue;
        }
        switch (nat_joystick(ev.code)) {
        case 'U': if (s->row > 0) { --s->row; dirty = 1; } break;
        case 'D': if (s->row + 1 < GRID_ROWS) { ++s->row; dirty = 1; } break;
        case 'L': if (s->col > 0) { --s->col; dirty = 1; } break;
        case 'R': if (s->col + 1 < GRID_COLS) { ++s->col; dirty = 1; } break;
        default: break;
        }
    }
    nat_input_close(&in);
    nat_fb_close(&fb);
    return 0;
}

static unsigned count_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') ++n;
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *data = getenv("NATARTOS_APP_DATA");
    const char *package = getenv("NATARTOS_APP_PACKAGE");
    if (!data || !package || getenv("LD_PRELOAD") || getenv("LD_LIBRARY_PATH")) return 1;
    hello_state s = {0};
    FILE *message = fopen("assets/message.txt", "rb");
    if (!message) { perror("message asset"); return 1; }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), message)) {
        fputs(buffer, stdout);
        if (!s.message[0]) {
            snprintf(s.message, sizeof(s.message), "%s", buffer);
            s.message[strcspn(s.message, "\r\n")] = 0;
        }
    }
    int read_error = ferror(message);
    fclose(message);
    if (read_error) return 1;
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/launches.txt", data) >= (int)sizeof(path)) return 1;
    FILE *saved = fopen(path, "ab");
    if (!saved) { perror("saved data"); return 1; }
    fputs("Hello Goggles launched successfully.\n", saved);
    if (fclose(saved)) return 1;
    s.launches = count_lines(path);
    s.source = getenv("NATARTOS_APP_SOURCE");
    printf("package=%s\ndata=%s\nsource=%s\n%s\n", package, data, s.source ? s.source : "(unknown)",
           source_place(s.source));
    for (int i=1; i<argc; ++i) printf("arg[%d]=%s\n", i, argv[i]);
    fflush(stdout);
    int rc = 0;
    if (getenv("NATARTOS_FB_PATH")) {
        rc = run_display(&s);
        printf("presses=%u\n", s.presses);
    }
    if (!rc) puts("Hello Goggles completed.");
    return rc;
}
