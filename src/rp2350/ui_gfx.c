/*
 * frank-castle — the settings window's drawing primitives.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * frank-amiga's ui_gfx.c (pico-speccy's OSD rasteriser by way of
 * frank-dino, with the Workbench 2/3 imagery added there), drawing BGR555
 * pixels in the colours of Symphony of the Night's menus (see ui_gfx.h).
 */
#include "ui_gfx.h"

#include <string.h>

#include "fbcon.h"

static const uint16_t pal[C_COUNT] = {
    [C_BG] = BGR555(0, 0, 0),
    [C_PANEL] = BGR555(16, 20, 72),
    [C_WELL] = BGR555(6, 8, 32),
    [C_PANEL_ALT] = BGR555(24, 28, 80),
    [C_TEXT] = BGR555(232, 232, 248),
    [C_TEXT_DIM] = BGR555(136, 144, 192),
    [C_WHITE] = BGR555(255, 255, 255),
    [C_SHINE] = BGR555(200, 208, 240),
    [C_SHADOW] = BGR555(4, 4, 16),
    [C_SEL_BG] = BGR555(64, 72, 176),
    [C_SEL_LT] = BGR555(144, 152, 232),
    [C_SEL_BAND] = BGR555(120, 16, 24),
    [C_ACCENT] = BGR555(255, 72, 64),
    [C_DISABLED] = BGR555(88, 92, 128),
    [C_BAR] = BGR555(136, 8, 16),
    [C_GOLD] = BGR555(240, 200, 104),
    [C_KNOB] = BGR555(150, 158, 200),
};

static uint16_t *fb;
extern void Video_PsramPace(void); /* the screen is in PSRAM, being scanned out */

void ui_begin(uint16_t *surface) { fb = surface; }

static inline void px(int x, int y, ui_color_t c) {
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) return;
    fb[y * UI_W + x] = pal[c];
}

void ui_hline(int x, int y, int w, ui_color_t c) {
    if (y < 0 || y >= UI_H || w <= 0) return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > UI_W) w = UI_W - x;
    if (w > 16) Video_PsramPace();
    uint16_t *p = fb + y * UI_W + x;
    const uint16_t v = pal[c];
    for (int i = 0; i < w; i++) p[i] = v;
}

void ui_vline(int x, int y, int h, ui_color_t c) {
    for (int i = 0; i < h; i++) px(x, y + i, c);
}

void ui_fill(int x, int y, int w, int h, ui_color_t c) {
    for (int i = 0; i < h; i++) ui_hline(x, y + i, w, c);
}

void ui_frame(int x, int y, int w, int h, ui_color_t c) {
    if (w <= 0 || h <= 0) return;
    ui_hline(x, y, w, c);
    ui_hline(x, y + h - 1, w, c);
    ui_vline(x, y, h, c);
    ui_vline(x + w - 1, y, h, c);
}

/* A shade between a and b (BGR555) at row i of n, ordered-dithered by
 * (x, y): 5 bits a channel band visibly over a window's height otherwise. */
static uint16_t shade(uint16_t a, uint16_t b, int i, int n, int x, int y) {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int t = bayer[y & 3][x & 3]; /* 0..15: sixteenths of a step */
    uint16_t out = 0;
    for (int sh = 0; sh <= 10; sh += 5) {
        const int c0 = (a >> sh) & 31, c1 = (b >> sh) & 31;
        int v = (c0 * 16 * (n - i) + c1 * 16 * i) / n; /* sixteenths */
        v = (v + t) >> 4;
        out |= (uint16_t)((v > 31 ? 31 : v) << sh);
    }
    return out;
}

/* the window's body shading (ui_window), for what is drawn on it */
#define BODY_TOP BGR555(14, 18, 66)
#define BODY_BOTTOM BGR555(40, 44, 128)
static int body_y0, body_h = 1;

static uint16_t body_at(int x, int y) {
    const int i = y < body_y0 ? 0 : y >= body_y0 + body_h ? body_h - 1 : y - body_y0;
    return shade(BODY_TOP, BODY_BOTTOM, i, body_h > 1 ? body_h - 1 : 1, x, y);
}

void ui_gradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom) {
    if (h <= 0) return;
    const int d = h > 1 ? h - 1 : 1;
    for (int i = 0; i < h; i++) {
        const int yy = y + i;
        if (yy < 0 || yy >= UI_H) continue;
        Video_PsramPace();
        for (int k = 0; k < w; k++) {
            const int xx = x + k;
            if (xx >= 0 && xx < UI_W) fb[yy * UI_W + xx] = shade(top, bottom, i, d, xx, yy);
        }
    }
}

/* ---- text ---- */

/* emboldened, Topaz-style: each pixel also to its right */
static void glyph(int x, int y, char ch, ui_color_t ink) {
    const uint8_t *g = ui_glyph(ch);
    Video_PsramPace();
    for (int r = 0; r < UI_FONT_H; r++) {
        const uint8_t bits = (uint8_t)(g[r] | g[r] >> 1);
        if (!bits) continue;
        for (int b = 0; b < 6; b++)
            if (bits & (0x20 >> b)) px(x + b, y + r, ink);
    }
}

int ui_text_width(const char *s) { return s ? (int)strlen(s) * UI_FONT_W : 0; }

int ui_text(int x, int y, const char *s, ui_color_t ink) {
    if (!s) return 0;
    int cx = x;
    for (; *s && cx < UI_W; s++, cx += UI_FONT_W)
        if (*s != ' ') glyph(cx, y, *s, ink);
    return cx - x;
}

int ui_text_right(int x1, int y, const char *s, ui_color_t ink) {
    return ui_text(x1 - ui_text_width(s), y, s, ink);
}

int ui_text_center(int x0, int x1, int y, const char *s, ui_color_t ink) {
    return ui_text((x0 + x1 - ui_text_width(s)) / 2, y, s, ink);
}

int ui_text_clip(int x, int y, int maxw, const char *s, ui_color_t ink) {
    if (!s || maxw <= 0) return 0;
    const int fits = maxw / UI_FONT_W, len = (int)strlen(s);
    if (len <= fits) return ui_text(x, y, s, ink);
    int keep = fits - 2 < 0 ? 0 : fits - 2, cx = x;
    for (int i = 0; i < keep; i++, cx += UI_FONT_W)
        if (s[i] != ' ') glyph(cx, y, s[i], ink);
    for (int i = 0; i < 2 && cx < x + maxw; i++, cx += UI_FONT_W) glyph(cx, y, '.', ink);
    return cx - x;
}

/* ---- the imagery ---- */

/* A bevel box, raised or recessed. Left and right edges are two pixels
 * wide, as GadTools draws them on a hires screen. */
void ui_bevel(int x, int y, int w, int h, bool raised) {
    if (w < 4 || h < 2) return;
    const ui_color_t lt = raised ? C_SHINE : C_SHADOW;
    const ui_color_t dk = raised ? C_SHADOW : C_SHINE;
    ui_hline(x, y, w - 1, lt);
    ui_vline(x, y, h - 1, lt);
    ui_vline(x + 1, y, h - 1, lt);
    ui_hline(x + 1, y + h - 1, w - 1, dk);
    ui_vline(x + w - 1, y, h, dk);
    ui_vline(x + w - 2, y + 1, h - 1, dk);
}

/* A frame line on all four sides: one row at the top and bottom, two
 * columns at the sides (a hires pixel is half as wide as it is tall). */
static void rect(int x, int y, int w, int h, ui_color_t c) {
    if (w < 4 || h < 2) return;
    ui_hline(x, y, w, c);
    ui_hline(x, y + h - 1, w, c);
    ui_vline(x, y, h, c);
    ui_vline(x + 1, y, h, c);
    ui_vline(x + w - 2, y, h, c);
    ui_vline(x + w - 1, y, h, c);
}

/* SOTN's boxes: a silver line all round, a dark one inside it */
void ui_well(int x, int y, int w, int h, ui_color_t fill) {
    ui_fill(x + 4, y + 2, w - 8, h - 4, fill);
    rect(x, y, w, h, C_TEXT_DIM);
    rect(x + 2, y + 1, w - 4, h - 2, C_SHADOW);
}

void ui_group(int x, int y, int w, int h, const char *title) {
    rect(x, y, w, h, C_SHINE);
    rect(x + 2, y + 1, w - 4, h - 2, C_SHADOW);
    if (title) {
        const int tw = ui_text_width(title);
        for (int r = 0; r < UI_FONT_H; r++) {
            const int yy = y - 4 + r;
            if (yy >= 0 && yy < UI_H)
                for (int k = 0; k < tw + 8; k++) fb[yy * UI_W + x + 12 + k] = body_at(x + 12 + k, yy);
        }
        ui_text(x + 16, y - 4, title, C_GOLD);
    }
}

void ui_button(int x, int y, int w, int h, const char *label, bool sel) {
    ui_fill(x + 2, y + 1, w - 4, h - 2, sel ? C_SEL_BG : C_PANEL_ALT);
    if (sel) ui_hline(x + 2, y + 1, w - 4, C_SEL_LT);
    ui_bevel(x, y, w, h, true);
    ui_text_center(x, x + w, y + (h - UI_FONT_H) / 2 + 1, label, sel ? C_WHITE : C_TEXT);
}

/* GadTools' MX (radio) knob, 16x9: raised; chosen, recessed with a gold dot */
void ui_mx(int x, int y, bool on) {
    ui_fill(x + 2, y + 1, 12, 7, on ? C_WELL : C_PANEL_ALT);
    ui_bevel(x, y, 16, 9, !on);
    if (on) ui_fill(x + 5, y + 3, 6, 3, C_GOLD);
}

/* a gold stud: the ends of the title bar */
static void stud(int x, int y) {
    ui_hline(x + 1, y, 2, C_GOLD);
    ui_hline(x, y + 1, 4, C_GOLD);
    ui_hline(x, y + 2, 4, C_GOLD);
    ui_hline(x + 1, y + 3, 2, C_GOLD);
    ui_hline(x + 1, y + 1, 1, C_WHITE);
}

void ui_window(int x0, int y0, int x1, int y1, const char *title, const char *right) {
    const int w = x1 - x0, h = y1 - y0;
    ui_fill(x0 + 6, y0 + 3, w, h, C_BG); /* drop shadow */
    body_y0 = y0;
    body_h = h;
    ui_gradient(x0, y0, w, h, BODY_TOP, BODY_BOTTOM);
    /* SOTN's frame: a silver line, a dark one, a dim silver one */
    rect(x0, y0, w, h, C_SHINE);
    rect(x0 + 2, y0 + 1, w - 4, h - 2, C_SHADOW);
    rect(x0 + 4, y0 + 2, w - 8, h - 4, C_TEXT_DIM);
    /* the title bar: crimson shading down to dark blood */
    const int th = UI_FONT_H + 3, tx = x0 + 6, ty = y0 + 3, tw = w - 12;
    ui_gradient(tx, ty, tw, th, BGR555(176, 20, 28), BGR555(72, 0, 8));
    ui_hline(tx, ty + th, tw, C_SHADOW);
    ui_hline(tx, ty + th + 1, tw, C_GOLD);
    if (title) ui_text(tx + 14, ty + 2, title, C_GOLD);
    if (right) ui_text_right(tx + tw - 14, ty + 2, right, C_TEXT);
    stud(tx + 3, ty + 4);
    stud(tx + tw - 7, ty + 4);
}
