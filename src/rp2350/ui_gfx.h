/*
 * frank-castle — the settings window's drawing: the window of murmheretic's
 * F12 settings (frank-amiga's prefs editor: a title bar, a listview, a
 * group frame, MX buttons, a slider, a status field) in the colours of
 * Symphony of the Night's own menus: deep blue panels shading down, thin
 * silver frames, gold for values, a crimson title bar. 640x240 hires
 * surface (8x10 character cells), over the dimmed game picture.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef CASTLE_UI_GFX_H
#define CASTLE_UI_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_font.h"

#define UI_W 640
#define UI_H 240

/* the UI symbols in the font */
#define SYM_UP "\x80"
#define SYM_DOWN "\x81"
#define SYM_RIGHT "\x82"
#define SYM_LEFT "\x83"
#define SYM_ENTER "\x84"
#define SYM_CHEV "\x85"

typedef enum {
    C_BG,         /* black */
    C_PANEL,      /* the window body (its flat colour; ui_window shades it) */
    C_WELL,       /* the listview and the status field: recessed, darker */
    C_PANEL_ALT,  /* the slider track */
    C_TEXT,       /* white, a little blue */
    C_TEXT_DIM,   /* grey-blue: help, levels above in the breadcrumb */
    C_WHITE,      /* text on the selection */
    C_SHINE,      /* silver: frames, the lit edge of a bevel */
    C_SHADOW,     /* its dark edge */
    C_SEL_BG,     /* the selection */
    C_SEL_LT,     /* the selection's top edge */
    C_SEL_BAND,   /* the selection while a key is awaited */
    C_ACCENT,     /* status messages, "press a key": blood red */
    C_DISABLED,
    C_BAR,        /* the title bar */
    C_GOLD,       /* the title, values */
    C_KNOB,       /* the slider knob, keycaps */
    C_COUNT
} ui_color_t;

/* Draw into this (shell_hires()) until the next ui_begin. */
void ui_begin(uint16_t *surface);

void ui_fill(int x, int y, int w, int h, ui_color_t c);
void ui_hline(int x, int y, int w, ui_color_t c);
void ui_vline(int x, int y, int h, ui_color_t c);
void ui_frame(int x, int y, int w, int h, ui_color_t c);
/* rows shading from `top` to `bottom` (BGR555) */
void ui_gradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom);

/* Text, emboldened like Topaz; returns the width drawn. */
int ui_text(int x, int y, const char *s, ui_color_t ink);
int ui_text_width(const char *s);
int ui_text_clip(int x, int y, int maxw, const char *s, ui_color_t ink);
int ui_text_right(int x1, int y, const char *s, ui_color_t ink);
int ui_text_center(int x0, int x1, int y, const char *s, ui_color_t ink);

void ui_bevel(int x, int y, int w, int h, bool raised);
/* a recessed field filled with `fill` */
void ui_well(int x, int y, int w, int h, ui_color_t fill);
/* a group frame (silver ridge) with its title cut into the top edge */
void ui_group(int x, int y, int w, int h, const char *title);
void ui_button(int x, int y, int w, int h, const char *label, bool sel);
void ui_mx(int x, int y, bool on);
/* The window: drop shadow, shaded body, SOTN's triple silver frame, the
 * title bar (`title` left, `right` right, gold studs at its ends). */
void ui_window(int x0, int y0, int x1, int y1, const char *title, const char *right);

#endif
