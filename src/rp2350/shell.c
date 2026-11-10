/*
 * frank-castle — the firmware's own screens: a welcome screen (the boot log
 * in a glass panel over a night scene: the moon, Dracula's castle, bats
 * flying across the sky), the error screens (no SD card, no game disc, a
 * fatal error) and the settings window's surface.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The screens are 320x240 BGR555
 * frames in PSRAM that the scanout shows instead of the game's VRAM
 * (Video_SetOverlay). There are two of them: everything is drawn into the
 * one not on screen, which is then shown from the next vblank and copied
 * back, so the screen never shows a half-drawn frame (no flicker when a
 * page is redrawn).
 */
#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/regs/addressmap.h"
#include "pico/stdlib.h"

#include "fbcon.h"
#include "font6x8.h"

#ifndef CASTLE_VERSION
#define CASTLE_VERSION "?"
#endif

extern void Video_SetOverlay(const uint16_t *fb, int width);
extern void Video_PsramPace(void);

#define W 320
#define H 240

/* colours (BGR555) */
#define C_BG BGR555(0, 0, 0)
#define C_TEXT BGR555(228, 228, 244)  /* white, a little blue */
#define C_HI BGR555(240, 200, 104)    /* gold */
#define C_ERR BGR555(255, 72, 64)
#define C_DIM BGR555(150, 156, 200)
#define C_FRAME BGR555(200, 208, 240) /* silver, as SOTN's menus */
#define C_FRAME_DIM BGR555(120, 126, 176)
#define C_ERRBAR BGR555(136, 8, 16)
#define C_WHITE BGR555(255, 255, 255)
#define C_SHADOW BGR555(4, 2, 12)
#define C_NIGHT BGR555(16, 10, 30)    /* the castle against the sky */
#define C_WINDOW BGR555(255, 170, 64)
#define C_BAT BGR555(40, 22, 56)       /* the body */
#define C_BAT_WING BGR555(86, 58, 116) /* the wings, lit by the moon */
#define C_BAT_EYE BGR555(255, 40, 32)

/* layout */
#define PX0 18
#define PY0 64
#define PX1 (W - 18)
#define PY1 214
#define TX0 (PX0 + 8)
#define TX1 (PX1 - 8)
#define TITLE_Y 6
#define SUB2_Y 42
#define SUB_Y (PY0 + 7)
#define SEP1_Y (SUB_Y + 21)
#define FOOT_Y (PY1 - 6 - 30)
#define SEP2_Y (FOOT_Y - 5)
#define LOG_COLS ((TX1 - TX0) / 6)

/* the two screens; `scr` is the one being drawn, never the one shown. The
 * welcome and error screens are 320 wide, the settings 640 (hires: the
 * settings window's 8x10 cells); `sw` is the width in use. */
#define HW 640
static uint16_t fbs[2][HW * H] __attribute__((section(".psram_bss.shell"), aligned(64)));
/* the night scene without the bats, which fly over it */
static uint16_t scene[W * H] __attribute__((section(".psram_bss.shell"), aligned(64)));
/* the title's pixels: the bats fly behind them */
static uint8_t front[W * H / 8] __attribute__((section(".psram_bss.shell")));
static int back, sw = W;
static uint16_t *scr;
static bool dirty;
bool shell_active;

static int log_y0, log_rows, log_col, log_row;
static uint32_t shown_us;

/* Show what was drawn: flip at the next vblank, then bring the other
 * buffer up to date so drawing can go on there. */
static void present(void) {
    if (!dirty) return;
    Video_SetOverlay(fbs[back], sw);
    /* row by row, letting the scanout's own PSRAM reads through */
    for (int y = 0; y < H; y++) {
        Video_PsramPace();
        memcpy(fbs[back ^ 1] + y * sw, fbs[back] + y * sw, (size_t)sw * 2);
    }
    back ^= 1;
    scr = fbs[back];
    dirty = false;
    shell_active = true;
}

static uint32_t rng = 0x6d2b79f5u;

static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ---- Bats, over the night sky (never over the panel) ---- */

#define NBATS 9
#define BAT_W 13 /* the art; drawn twice the size */
#define BAT_H 6
#define BAT_S 2

typedef struct {
    int16_t x, y;      /* top left, 1/16 pixels */
    int16_t vx;        /* 1/16 pixels a frame, the sign is the direction */
    uint8_t phase;     /* of the wing beat and the bob */
    uint8_t wait;      /* frames before it comes in */
    int16_t drawn_x, drawn_y; /* where it was drawn, pixels; -1000: nowhere */
} bat_t;

static bat_t bats[NBATS];
static bool bats_on;

/* two frames of a wing beat: wings up, wings down ('#' body, '+' wing,
 * 'o' eye) */
static const char *const bat_art[2][BAT_H] = {
    {"++         ++", " +++  #  +++ ", "  +++#o#+++  ", "    +###+    ", "     # #     ", "             "},
    {"             ", "     # #     ", "   ++#o#++   ", " ++++###++++ ", "++   +#+   ++", "+     #     +"},
};

static void bat_spawn(bat_t *b) {
    const uint32_t r = rnd();
    const bool right = r & 1;
    const int speed = 10 + (int)((r >> 4) % 18);
    b->vx = (int16_t)(right ? speed : -speed);
    b->x = (int16_t)((right ? -BAT_W * BAT_S - (int)((r >> 9) % 40) : W + (int)((r >> 9) % 40)) * 16);
    /* the sky above the panel */
    b->y = (int16_t)((2 + (int)((r >> 16) % 40)) * 16);
    b->phase = (uint8_t)(r >> 24);
    b->wait = (uint8_t)((r >> 12) % 90);
}

static inline bool in_panel(int x, int y) { return x >= PX0 && x < PX1 && y >= PY0 && y < PY1; }
static inline bool in_front(int x, int y) { return front[(y * W + x) >> 3] & (1u << (x & 7)); }

/* the scene back where a bat was */
static void bat_erase(bat_t *b) {
    if (b->drawn_x <= -1000) return;
    for (int r = 0; r < (BAT_H + 3) * BAT_S; r++) {
        const int y = b->drawn_y + r;
        if (y < 0 || y >= H) continue;
        for (int k = 0; k < BAT_W * BAT_S; k++) {
            const int x = b->drawn_x + k;
            if (x < 0 || x >= W || in_panel(x, y)) continue;
            scr[y * W + x] = scene[y * W + x];
        }
    }
    b->drawn_x = -1000;
}

static void bat_draw(bat_t *b) {
    const int x0 = b->x >> 4;
    /* a slow bob, a little out of step with the wings */
    static const int8_t bob[16] = {0, 1, 1, 2, 2, 2, 1, 1, 0, 0, -1, -1, -1, -1, 0, 0};
    const int y0 = (b->y >> 4) + bob[(b->phase >> 2) & 15] * BAT_S + 2;
    const char *const *art = bat_art[(b->phase >> 2) & 1];
    for (int r = 0; r < BAT_H * BAT_S; r++) {
        const int y = y0 + r;
        if (y < 0 || y >= H) continue;
        for (int k = 0; k < BAT_W * BAT_S; k++) {
            const int ak = k / BAT_S;
            const char c = art[r / BAT_S][b->vx > 0 ? ak : BAT_W - 1 - ak];
            const int x = x0 + k;
            if (c == ' ' || x < 0 || x >= W || in_panel(x, y) || in_front(x, y)) continue;
            scr[y * W + x] = c == 'o' ? C_BAT_EYE : c == '+' ? C_BAT_WING : C_BAT;
        }
    }
    b->drawn_x = (int16_t)x0;
    b->drawn_y = (int16_t)(y0 - 2);
}

static void bats_frame(void) {
    for (int i = 0; i < NBATS; i++) bat_erase(&bats[i]);
    for (int i = 0; i < NBATS; i++) {
        bat_t *b = &bats[i];
        if (b->wait) {
            b->wait--;
            continue;
        }
        b->x = (int16_t)(b->x + b->vx);
        b->phase++;
        if ((b->phase & 7) == 0) b->y = (int16_t)(b->y + (int)(rnd() % 3) * 8 - 8); /* flutter */
        if (b->y < 0) b->y = 0;
        if (b->y > (PY0 - BAT_H * BAT_S - 8) * 16) b->y = (int16_t)((PY0 - BAT_H * BAT_S - 8) * 16); /* above the panel */
        const int x = b->x >> 4;
        if ((b->vx > 0 && x > W + 4) || (b->vx < 0 && x < -BAT_W * BAT_S - 4)) {
            bat_spawn(b);
            continue;
        }
        bat_draw(b);
    }
    dirty = true;
}

static void bats_start(void) {
    for (int i = 0; i < NBATS; i++) {
        bat_spawn(&bats[i]);
        bats[i].drawn_x = -1000;
        /* some already in the sky */
        if (i < 4) {
            bats[i].wait = 0;
            bats[i].x = (int16_t)((int)(rnd() % W) * 16);
        }
    }
    bats_on = true;
}

void shell_tick(void) {
    if (!scr) return;
    if (bats_on && sw == W) bats_frame();
    if (dirty) {
        present(); /* waits for the vblank */
    } else {
        sleep_ms(16);
    }
}

uint32_t shell_shown_ms(void) { return (time_us_32() - shown_us) / 1000u; }

/* ---- Drawing ---- */

static void fill(int x0, int y0, int x1, int y1, uint16_t c) {
    for (int y = y0; y < y1; y++) {
        Video_PsramPace();
        uint16_t *p = scr + y * W;
        for (int x = x0; x < x1; x++) p[x] = c;
    }
    dirty = true;
}

static inline void put(int x, int y, uint16_t c) {
    if (x >= 0 && x < W && y >= 0 && y < H) scr[y * W + x] = c;
}

static void glyph(int x, int y, char ch, uint16_t c) {
    unsigned u = (unsigned char)ch;
    const uint8_t *g = font6x8[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
    for (int r = 0; r < 8; r++)
        for (int k = 0; k < 6; k++)
            if (g[r] & (0x80 >> k)) scr[(y + r) * W + x + k] = c;
    dirty = true;
}

static void text(int x, int y, const char *s, uint16_t c) {
    for (; *s; s++, x += 6) glyph(x, y, *s, c);
}

static void text_center(int y, const char *s, uint16_t c) { text((W - 6 * (int)strlen(s)) / 2, y, s, c); }

static uint16_t rgb_mix(int r0, int g0, int b0, int r1, int g1, int b1, int i, int n) {
    return BGR555(r0 + (r1 - r0) * i / n, g0 + (g1 - g0) * i / n, b0 + (b1 - b0) * i / n);
}

/* Rows of the title: the lavender of the game's logo, darker down; of the
 * subtitle: silver. */
static uint16_t title_col(int row) { return rgb_mix(214, 190, 255, 104, 72, 176, row, 31); }
static uint16_t sub_col(int row) { return rgb_mix(236, 236, 248, 150, 150, 196, row, 15); }

/* Text scaled up: a dark outline all round, then the gradient by row. */
static void big_text(int x0, int y0, const char *s, int scale, uint16_t (*col)(int), uint16_t outline) {
    const int n = (int)strlen(s);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            const uint8_t *g = font6x8[(unsigned char)s[i] - 32];
            for (int r = 0; r < 8; r++)
                for (int k = 0; k < 6; k++) {
                    if (!(g[r] & (0x80 >> k))) continue;
                    const int x = x0 + (i * 6 + k) * scale, y = y0 + r * scale;
                    if (!pass) { /* the outline, and a shadow down right */
                        for (int dy = -1; dy <= scale + 1; dy++)
                            for (int dx = -1; dx <= scale + 1; dx++) {
                                const int px = x + dx, py = y + dy;
                                put(px, py, outline);
                                if (px >= 0 && px < W && py >= 0 && py < H)
                                    front[(py * W + px) >> 3] |= (uint8_t)(1u << (px & 7));
                            }
                        continue;
                    }
                    for (int dy = 0; dy < scale; dy++) {
                        const uint16_t c = col(r * scale + dy);
                        for (int dx = 0; dx < scale; dx++) put(x + dx, y + dy, c);
                    }
                }
        }
    dirty = true;
}

/* ---- The night scene ---- */

#define MOON_X 296
#define MOON_Y 27
#define MOON_R 19

static void sky(void) {
    /* deep blue at the top, violet down to the horizon */
    static const uint8_t bayer[4][4] = {{0, 4, 1, 5}, {6, 2, 7, 3}, {1, 5, 0, 4}, {7, 3, 6, 2}};
    for (int y = 0; y < H; y++) {
        /* 8-bit channels, ordered-dithered down to 5: no bands */
        int r, g, b;
        if (y < 150) {
            r = 2 + 32 * y / 150;
            g = 2 + 16 * y / 150;
            b = 18 + 52 * y / 150;
        } else {
            r = 34 + 36 * (y - 150) / 90;
            g = 18 + 12 * (y - 150) / 90;
            b = 70 + 12 * (y - 150) / 90;
        }
        Video_PsramPace();
        uint16_t *p = scr + y * W;
        for (int x = 0; x < W; x++) {
            const int d = bayer[y & 3][x & 3];
            p[x] = BGR555(r + d > 255 ? 255 : r + d, g + d > 255 ? 255 : g + d, b + d > 255 ? 255 : b + d);
        }
    }
    /* stars: fixed, some brighter */
    uint32_t s = 0x9e3779b9u;
    for (int i = 0; i < 90; i++) {
        s = s * 1664525u + 1013904223u;
        const int x = (int)(s >> 8) % W, y = (int)(s >> 20) % 170;
        const int b = 110 + (int)((s >> 4) & 127);
        put(x, y, BGR555(b, b, b > 200 ? 255 : b + 30));
        if ((s & 0x1f) == 0) { /* a bright one with a cross */
            put(x - 1, y, BGR555(90, 90, 130));
            put(x + 1, y, BGR555(90, 90, 130));
            put(x, y - 1, BGR555(90, 90, 130));
            put(x, y + 1, BGR555(90, 90, 130));
        }
    }
    /* the moon: a halo, then the disc, its seas a little darker */
    for (int dy = -MOON_R - 10; dy <= MOON_R + 10; dy++)
        for (int dx = -MOON_R - 10; dx <= MOON_R + 10; dx++) {
            const int d2 = dx * dx + dy * dy, x = MOON_X + dx, y = MOON_Y + dy;
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            if (d2 <= MOON_R * MOON_R) {
                int v = 236 - (dx + dy) / 2; /* lit from the top left */
                const int sx = dx + 6, sy = dy - 4, tx = dx - 7, ty = dy + 6, ux = dx + 2, uy = dy + 11;
                if (sx * sx + sy * sy < 30 || tx * tx + ty * ty < 22 || ux * ux + uy * uy < 12) v -= 28;
                if (v > 255) v = 255;
                put(x, y, BGR555(v, v - 8, v - 40));
            } else {
                const int rr = MOON_R + 10;
                if (d2 > rr * rr) continue;
                const uint16_t p = scr[y * W + x];
                /* add a faint glow */
                const int k = (rr * rr - d2) * 12 / (rr * rr - MOON_R * MOON_R);
                int r = (p & 31) + k / 2, g = ((p >> 5) & 31) + k / 2, b = ((p >> 10) & 31) + k / 3;
                scr[y * W + x] = (uint16_t)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (b > 31 ? 31 : b) << 10);
            }
        }
}

typedef struct {
    int16_t x, w, top; /* a tower: left, width, top of its wall */
    int8_t spire;      /* a pointed roof this tall, 0: battlements */
} tower_t;

/* Dracula's castle along the bottom, tall towers in the margins beside the
 * panel */
static const tower_t towers[] = {
    {0, 14, 120, 22},  {12, 10, 150, 0},  {300, 12, 112, 26}, {290, 12, 156, 0}, {22, 30, 196, 0},
    {48, 12, 182, 14}, {60, 44, 204, 0},  {100, 10, 176, 18}, {112, 50, 200, 0}, {160, 16, 168, 24},
    {176, 46, 202, 0}, {222, 12, 180, 16}, {234, 40, 198, 0}, {272, 18, 186, 0},
};

static void castle(void) {
    for (unsigned t = 0; t < sizeof(towers) / sizeof(towers[0]); t++) {
        const tower_t *T = &towers[t];
        for (int y = T->top; y < H; y++)
            for (int x = T->x; x < T->x + T->w; x++) put(x, y, C_NIGHT);
        if (T->spire) {
            for (int r = 0; r < T->spire; r++) { /* the roof, and a thin spike */
                const int half = (T->w / 2 + 1) * r / T->spire;
                for (int x = T->x + T->w / 2 - half; x <= T->x + T->w / 2 + half; x++)
                    put(x, T->top - T->spire + r, C_NIGHT);
            }
            for (int r = 1; r < 5; r++) put(T->x + T->w / 2, T->top - T->spire - r, C_NIGHT);
        } else {
            for (int x = T->x; x < T->x + T->w; x += 4) /* battlements */
                for (int r = 1; r <= 3; r++) {
                    put(x, T->top - r, C_NIGHT);
                    put(x + 1, T->top - r, C_NIGHT);
                }
        }
        /* lit windows: tall slits, here and there */
        uint32_t s = 0x2545f491u * (t + 1);
        for (int y = T->top + 6; y < H - 6; y += 13)
            for (int x = T->x + 3; x + 2 < T->x + T->w - 2; x += 7) {
                s = s * 1664525u + 1013904223u;
                if ((s >> 28) > 3) continue;
                for (int r = 0; r < 4; r++) {
                    put(x, y + r, C_WINDOW);
                    put(x + 1, y + r, r ? C_WINDOW : BGR555(200, 110, 40));
                }
            }
    }
}

/* the panel: the night seen through dark glass, SOTN's silver frame */
static void panel(void) {
    for (int y = PY0; y < PY1; y++) {
        Video_PsramPace();
        for (int x = PX0; x < PX1; x++) {
            const uint16_t p = scr[y * W + x];
            /* a quarter of the scene, over deep blue */
            const int r = (p & 31) / 4 + 1, g = ((p >> 5) & 31) / 4 + 1, b = ((p >> 10) & 31) / 4 + 5;
            scr[y * W + x] = (uint16_t)(r | g << 5 | b << 10);
        }
    }
    for (int i = 0; i < 3; i++) {
        const uint16_t c = i == 0 ? C_FRAME : i == 1 ? C_SHADOW : C_FRAME_DIM;
        fill(PX0 + i, PY0 + i, PX1 - i, PY0 + i + 1, c);
        fill(PX0 + i, PY1 - i - 1, PX1 - i, PY1 - i, c);
        fill(PX0 + i, PY0 + i, PX0 + i + 1, PY1 - i, c);
        fill(PX1 - i - 1, PY0 + i, PX1 - i, PY1 - i, c);
    }
}

static void separator(int y) {
    fill(TX0, y, TX1, y + 1, C_SHADOW);
    fill(TX0, y + 1, TX1, y + 2, C_FRAME_DIM);
}

static void footer(const char *hint) {
    char line[64];
    if (hint) {
        text_center(FOOT_Y, hint, C_ERR);
    } else {
        const unsigned sys = (unsigned)(clock_get_hz(clk_sys) / 1000000);
        const unsigned psram =
            sys / ((qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >> QMI_M1_TIMING_CLKDIV_LSB);
        const unsigned flash =
            sys / ((qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB);
        snprintf(line, sizeof(line), "M2  CPU %u  PSRAM %u  FLASH %u MHz", sys, psram, flash);
        text_center(FOOT_Y, line, C_DIM);
    }
    text_center(FOOT_Y + 11, "Port (c) 2026 Mikhail Matveev", C_TEXT);
    text_center(FOOT_Y + 22, "github.com/rh1tech/frank-castle", C_HI);
}

/* The whole screen; with a title, a red bar over the log. */
static void screen(const char *bar, const char *hint) {
    if (!scr) {
        back = 0;
        scr = fbs[0];
    }
    sw = W;
    dirty = true;
    memset(front, 0, sizeof(front));
    sky();
    castle();
    /* left, the moon beside it */
    big_text(8, TITLE_Y, "CASTLEVANIA", 4, title_col, C_SHADOW);
    big_text(10, SUB2_Y, "SYMPHONY OF THE NIGHT", 2, sub_col, C_SHADOW);
    panel();
    /* the bats fly over this */
    memcpy(scene, scr, sizeof(scene));
    text_center(SUB_Y, "for RP2350  -  frank-castle " CASTLE_VERSION, C_TEXT);
    text_center(SUB_Y + 10, "Castlevania SOTN (c) 1997 Konami", C_DIM);
    separator(SEP1_Y);
    separator(SEP2_Y);
    log_y0 = SEP1_Y + 6;
    if (bar) {
        fill(TX0, log_y0, TX1, log_y0 + 12, C_ERRBAR);
        fill(TX0, log_y0, TX1, log_y0 + 1, C_ERR);
        text_center(log_y0 + 2, bar, C_WHITE);
        log_y0 += 16;
    }
    log_rows = (SEP2_Y - 3 - log_y0) / 9;
    log_col = log_row = 0;
    footer(hint);
    bats_start();
}

void shell_welcome(void) {
    screen(NULL, NULL);
    present();
    shown_us = time_us_32();
}

void shell_error(const char *bar, const char *hint) {
    screen(bar, hint);
    present();
}

/* The game starts drawing: black until its first frame (Video_HoldBlack,
 * released by Settings_Frame), not its VRAM half drawn */
void shell_handover(void) {
    extern void Video_HoldBlack(bool on);
    Video_HoldBlack(true);
    shell_release();
}

void shell_release(void) {
    if (!shell_active) return;
    Video_SetOverlay(NULL, 0);
    shell_active = false;
}

/* ---- The log ---- */

static void log_scroll(void) {
    for (int y = log_y0; y < log_y0 + 9 * (log_rows - 1); y++) {
        Video_PsramPace();
        memcpy(scr + y * W + TX0, scr + (y + 9) * W + TX0, (size_t)(TX1 - TX0) * 2);
    }
    for (int y = log_y0 + 9 * (log_rows - 1); y < log_y0 + 9 * log_rows; y++) /* the glass again */
        memcpy(scr + y * W + TX0, scene + y * W + TX0, (size_t)(TX1 - TX0) * 2);
    dirty = true;
    log_row = log_rows - 1;
}

static void log_newline(void) {
    log_col = 0;
    if (++log_row >= log_rows) log_scroll();
}

static void log_putc(char ch, uint16_t c) {
    if (ch == '\n') {
        log_newline();
        return;
    }
    if (log_col >= LOG_COLS) log_newline();
    glyph(TX0 + log_col * 6, log_y0 + log_row * 9, ch, c);
    log_col++;
}

static void log_text(uint16_t c, const char *buf) {
    if (!scr || sw != W) return; /* not while the settings are up */
    for (const char *s = buf; *s;) { /* word wrap */
        if (*s != ' ' && *s != '\n') {
            int n = 0;
            while (s[n] && s[n] != ' ' && s[n] != '\n') n++;
            if (log_col > 0 && log_col + n > LOG_COLS) log_newline();
            while (n--) log_putc(*s++, c);
        } else {
            if (*s == ' ' && log_col >= LOG_COLS)
                log_newline();
            else
                log_putc(*s, c);
            s++;
        }
    }
    present();
}

static void log_vprint(uint16_t c, const char *fmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fputs(buf, stdout);
    log_text(c, buf);
}

#define SHELL_PRINT(name, colour)        \
    void name(const char *fmt, ...) {    \
        va_list ap;                      \
        va_start(ap, fmt);               \
        log_vprint(colour, fmt, ap);     \
        va_end(ap);                      \
    }
SHELL_PRINT(shell_printf, C_TEXT)
SHELL_PRINT(shell_highlight, C_HI)
SHELL_PRINT(shell_errorf, C_ERR)
SHELL_PRINT(shell_dim, C_DIM)

/* ---- The settings (ui_gfx.c draws it) ---- */

uint16_t *shell_hires(void) {
    if (!scr) {
        back = 0;
        scr = fbs[0];
    }
    if (sw != HW) {
        sw = HW;
        memset(scr, 0, (size_t)sw * H * 2);
    }
    /* the settings window covers the screen: no bats over it */
    bats_on = false;
    dirty = true;
    return scr;
}

/* Show the page drawn so far (at the next vblank). */
void shell_show(void) {
    dirty = true;
    present();
}
