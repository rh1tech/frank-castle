/*
 * frank-castle — USB_HID=OFF (development builds): the native USB port is
 * the CDC console, so there is no keyboard or gamepad on it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "usbhid.h"

#include <string.h>

void usbhid_init(void) {}
void usbhid_task(void) {}
int usbhid_keyboard_connected(void) { return 0; }
int usbhid_mouse_connected(void) { return 0; }
int usbhid_gamepad_connected(void) { return 0; }
uint32_t usbhid_gamepad_buttons(void) { return 0; }
void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state) { memset(state, 0, sizeof(*state)); }
void usbhid_get_mouse_state(usbhid_mouse_state_t *state) { memset(state, 0, sizeof(*state)); }
int usbhid_get_key_action(uint8_t *keycode, int *down) {
    (void)keycode;
    (void)down;
    return 0;
}
