/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

/*
 * USB HID Host Driver Header
 * Provides keyboard and mouse input via USB Host
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USBHID_H
#define USBHID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// USB HID State
//--------------------------------------------------------------------

// USB keyboard state accessible from wrapper
typedef struct {
    uint8_t keycode[6];     // Currently pressed keys (HID keycodes)
    uint8_t modifier;       // Modifier keys (shift, ctrl, alt, etc.)
    int has_key;            // Non-zero if a key event is pending
} usbhid_keyboard_state_t;

// USB mouse state
typedef struct {
    int16_t dx;             // Accumulated X movement
    int16_t dy;             // Accumulated Y movement
    int8_t wheel;           // Wheel movement
    uint8_t buttons;        // Button state (bit 0=left, 1=right, 2=middle)
    int has_motion;         // Non-zero if motion/button change occurred
} usbhid_mouse_state_t;

// Gamepad buttons, by position (an Xbox pad's A is SOUTH, a PlayStation
// pad's cross is SOUTH). The decoders come from pico-speccy: XInput, the
// DualShock 4 and DualSense, the Logitech F710 in D mode, a few clones and
// DragonRise-style generic HID pads.
#define GP_UP     0x00001u
#define GP_DOWN   0x00002u
#define GP_LEFT   0x00004u
#define GP_RIGHT  0x00008u
#define GP_SOUTH  0x00010u
#define GP_EAST   0x00020u
#define GP_WEST   0x00040u
#define GP_NORTH  0x00080u
#define GP_L1     0x00100u
#define GP_R1     0x00200u
#define GP_L2     0x00400u
#define GP_R2     0x00800u
#define GP_START  0x01000u
#define GP_SELECT 0x02000u
#define GP_L3     0x04000u
#define GP_R3     0x08000u
#define GP_HOME   0x10000u
#define GP_COUNT  17

//--------------------------------------------------------------------
// API Functions
//--------------------------------------------------------------------

/** Buttons held on all connected gamepads together (GP_*). */
uint32_t usbhid_gamepad_buttons(void);

/** Non-zero while at least one gamepad is connected. */
int usbhid_gamepad_connected(void);

/**
 * Initialize USB Host HID driver
 * Call this during system initialization
 */
void usbhid_init(void);

/**
 * Poll USB Host for events
 * Must be called periodically (e.g., every frame)
 */
void usbhid_task(void);

/**
 * Check if a USB keyboard is connected
 * @return Non-zero if keyboard connected
 */
int usbhid_keyboard_connected(void);

/**
 * Check if a USB mouse is connected
 * @return Non-zero if mouse connected
 */
int usbhid_mouse_connected(void);

/**
 * Get current keyboard state
 * @param state Pointer to state structure to fill
 */
void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state);

/**
 * Get current mouse state and reset deltas
 * @param state Pointer to state structure to fill
 */
void usbhid_get_mouse_state(usbhid_mouse_state_t *state);

/**
 * Get pending key action (for wrapper compatibility)
 * @param keycode HID keycode of the key
 * @param down Non-zero if key pressed, 0 if released
 * @return Non-zero if action available
 */
int usbhid_get_key_action(uint8_t *keycode, int *down);

#ifdef __cplusplus
}
#endif

#endif /* USBHID_H */
