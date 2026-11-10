/*
 * frank-castle — input (USB keyboard and gamepads) and the settings menu.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef CASTLE_SETTINGS_H
#define CASTLE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* /castle/castle.cfg: read once the card is mounted, applied at once. */
void Settings_Load(void);

/* Drain the USB key events and the gamepads (game thread, every frame). */
void Input_Poll(void);
/* The PS1 pad buttons held (libetc's PADL and PADR bits), from the bindings. */
uint32_t Input_PadBits(void);
/* True once F12, a gamepad's Home button or Select+Start was pressed. */
bool Input_MenuRequested(void);

/* Ask for the menu from elsewhere (the UART's "~C menu~"). */
void Settings_Request(void);

/* The settings menu, modal. `in_game`: the game is paused under it. */
void Settings_Run(bool in_game);

/* F9 (debug): pressed since the last call. */
bool Settings_SkipRequested(void);
/* Game arguments F9 left for this boot (it restarts into the Castle
 * Entrance), or NULL. */
const char *Settings_BootArgs(void);
/* F9 on the welcome screen: this boot goes straight to the Castle Entrance. */
void Settings_SkipAtBoot(void);

/* Game thread, at the end of every VSync: input, and the menu if asked. */
void Settings_Frame(void);

#endif
