/*
 * Generic USB gamepad / joystick: decoded by its report descriptor.
 *
 * C port of pico-speccy's src/drivers/usbhost/HidPadLayout.h (drewpo28,
 * GPL-3.0-or-later, 2026-11-04). The fixed decoder (decode_generic in
 * hid_app.c) knows one layout only, the 8-byte DragonRise / SNES-style
 * report, and drops anything shorter than 7 bytes: every other Joystick /
 * Gamepad interface without a vendor handler was dead or decoded from the
 * wrong bytes (a Logitech Precision 046D:C21A sends 4 bytes: X, Y, 10
 * one-bit buttons, 6 vendor bits).
 *
 * So the descriptor is parsed at mount: X/Y (with their logical range), the
 * hat switch and the Button-page bits of the first report inside a
 * Desktop/Joystick or Desktop/Gamepad application collection. Directions
 * come from X/Y (the outer quarter of the logical range) or the hat;
 * buttons are numbered as the HID Button page (bit 0 = Button 1).
 *
 * Host test: tools/hid_pad_layout_test.c
 *   cc -O2 -Wall -Wextra -Idrivers/usbhid -o /tmp/hpl tools/hid_pad_layout_test.c \
 *      drivers/usbhid/hid_pad_layout.c && /tmp/hpl
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef HID_PAD_LAYOUT_H
#define HID_PAD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t off;        /* bit offset in the payload */
    uint8_t size;        /* bits, 0 = absent */
    int32_t lmin, lmax;  /* logical range */
} hid_pad_axis_t;

typedef struct {         /* cnt consecutive one-bit buttons from usage `first` */
    uint16_t off;
    uint8_t first;       /* HID button usage (1-based) */
    uint8_t cnt;
} hid_pad_run_t;

typedef struct {
    bool use;
    uint8_t report_id;   /* 0 = no report-id byte */
    uint16_t bytes;      /* payload size without the id byte */
    hid_pad_axis_t x, y, hat;
    uint8_t nruns;
    hid_pad_run_t runs[4];
} hid_pad_layout_t;

typedef struct {
    bool up, down, left, right;
    uint32_t buttons;    /* bit n = HID Button n+1 */
} hid_pad_report_t;

/* False (and out->use false) when the descriptor has no usable pad. */
bool hid_pad_parse(hid_pad_layout_t *out, const uint8_t *desc, uint16_t desc_len, uint16_t max_bytes);

/* False when the report is not the one the layout describes (another
 * report id, too short). `report` starts with the id byte if it has one. */
bool hid_pad_decode(const hid_pad_layout_t *L, const uint8_t *report, uint16_t len, hid_pad_report_t *out);

#endif
