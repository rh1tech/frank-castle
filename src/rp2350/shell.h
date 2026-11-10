/*
 * frank-castle — the firmware's own screens: welcome, errors, settings.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef CASTLE_SHELL_H
#define CASTLE_SHELL_H

#include <stdbool.h>
#include <stdint.h>

/* True while a shell screen is shown instead of the game. */
extern bool shell_active;

void shell_welcome(void);
/* An error screen: `title` in a red bar, `hint` in the footer. */
void shell_error(const char *title, const char *hint);
/* Back to the game's picture (from the next vblank). */
void shell_release(void);
/* The game's first drawing: the welcome goes, black until its first frame. */
void shell_handover(void);

/* Lines into the panel's log (and the UART). */
void shell_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void shell_highlight(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void shell_errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void shell_dim(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One frame: the drips move and whatever was drawn is shown, at vblank. */
void shell_tick(void);
/* Milliseconds since the welcome screen was drawn. */
uint32_t shell_shown_ms(void);

/* ---- the settings (settings.c, ui_gfx.c) ---- */

/* The 640x240 hires screen being drawn (not the one shown). */
uint16_t *shell_hires(void);
/* Show what was drawn, at the next vblank. */
void shell_show(void);

#endif
