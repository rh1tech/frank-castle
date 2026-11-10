/*
 * USB HID Host Application Callbacks
 * Implements TinyUSB Host callbacks for keyboard, mouse and gamepads
 *
 * Based on TinyUSB HID host example. The gamepad decoders (vendor layouts,
 * the RIP descriptor parser fallback, XInput) come from pico-speccy.
 * SPDX-License-Identifier: MIT
 */

#include "hid_pad_layout.h"
#include "tusb.h"
#include "usbhid.h"
#include "hid_rip.h"
#include "xinput_host.h"
#include <stdio.h>
#include <string.h>

// Only compile if USB Host is enabled
#if CFG_TUH_ENABLED

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------

#define MAX_REPORT 4

// Per-device, per-instance HID info for generic report parsing
// Index by dev_addr * CFG_TUH_HID + instance (simplified indexing)
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
    uint16_t vid, pid;
} hid_info[CFG_TUH_HID];

// Generic pads decoded by their descriptor (hid_pad_layout.h; parsed at mount
// for itf_protocol NONE interfaces, used by process_generic_report)
static hid_pad_layout_t pad_layout[CFG_TUH_HID];

// Gamepad buttons (GP_*) by HID instance and by XInput device
static volatile uint32_t gp_hid[CFG_TUH_HID];
static volatile uint32_t gp_xinput[CFG_TUH_DEVICE_MAX + 1];
static volatile uint8_t gp_is_pad[CFG_TUH_HID], gp_xinput_on[CFG_TUH_DEVICE_MAX + 1];

// Previous keyboard report for detecting key changes
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

// Previous mouse report for detecting button changes
static hid_mouse_report_t prev_mouse_report = { 0 };

// Accumulated mouse movement
static volatile int16_t cumulative_dx = 0;
static volatile int16_t cumulative_dy = 0;
static volatile int8_t cumulative_wheel = 0;
static volatile uint8_t current_buttons = 0;
static volatile int mouse_has_motion = 0;

// Device connection state
static volatile int keyboard_connected = 0;
static volatile int mouse_connected = 0;

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

//--------------------------------------------------------------------
// Internal functions
//--------------------------------------------------------------------

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

// Forward declarations
static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report);
static void process_mouse_report(hid_mouse_report_t const *report);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

//--------------------------------------------------------------------
// Process keyboard report
//--------------------------------------------------------------------

static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    // Handle modifier changes
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods = report->modifier & ~(prev_report->modifier);

    // Map modifier bits to pseudo-keycodes (using high byte to distinguish)
    // These will be translated in the wrapper
    if (released_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
        queue_key_action(0xE1, 0); // SHIFT released (HID Left Shift)
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
        queue_key_action(0xE1, 1); // SHIFT pressed
    }
    if (released_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
        queue_key_action(0xE0, 0); // CTRL released
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
        queue_key_action(0xE0, 1); // CTRL pressed
    }
    if (released_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
        queue_key_action(0xE2, 0); // ALT released
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
        queue_key_action(0xE2, 1); // ALT pressed
    }

    // Check for released keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode)) {
            queue_key_action(keycode, 0); // Key released
        }
    }

    // Check for pressed keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode)) {
            queue_key_action(keycode, 1); // Key pressed
        }
    }
}

//--------------------------------------------------------------------
// Process mouse report
//--------------------------------------------------------------------

static void process_mouse_report(hid_mouse_report_t const *report) {
    // Standard boot protocol mouse report
    // Note: Y axis inverted for Duke3D (positive Y = forward in game)
    cumulative_dx += report->x;
    cumulative_dy += -report->y;  // Invert Y for correct forward/back
    cumulative_wheel += report->wheel;
    current_buttons = report->buttons & 0x07;

    if (report->x != 0 || report->y != 0) {
        mouse_has_motion = 1;
    }

    prev_mouse_report = *report;
}


//--------------------------------------------------------------------
// Gamepads (layouts from pico-speccy's hid_app.cpp)
//--------------------------------------------------------------------

static inline int is_dualshock4(uint16_t vid, uint16_t pid) { return vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC); }
static inline int is_dualsense(uint16_t vid, uint16_t pid) { return vid == 0x054C && pid == 0x0CE6; }
static inline int is_f710_dinput(uint16_t vid, uint16_t pid) { return vid == 0x046D && pid == 0xC219; }
static inline int is_gp_2563_0575(uint16_t vid, uint16_t pid) { return vid == 0x2563 && pid == 0x0575; }
static inline int is_gp_feed_2320(uint16_t vid, uint16_t pid) { return vid == 0xFEED && pid == 0x2320; }
// DragonRise "Game Stick Lite": announces boot keyboard/mouse, sends joystick reports
static inline int is_gp_0810_0001(uint16_t vid, uint16_t pid) { return vid == 0x0810 && pid == 0x0001; }

// Left stick (8-bit, 0x80 centre) and hat (0 = up, clockwise, 8+ = neutral)
static uint32_t gp_dirs(uint8_t lx, uint8_t ly, int hat) {
    uint32_t b = 0;
    if (lx < 0x40) b |= GP_LEFT;
    if (lx > 0xC0) b |= GP_RIGHT;
    if (ly < 0x40) b |= GP_UP;
    if (ly > 0xC0) b |= GP_DOWN;
    if (hat >= 0 && hat < 8) {
        if (hat == 7 || hat == 0 || hat == 1) b |= GP_UP;
        if (hat >= 1 && hat <= 3) b |= GP_RIGHT;
        if (hat >= 3 && hat <= 5) b |= GP_DOWN;
        if (hat >= 5 && hat <= 7) b |= GP_LEFT;
    }
    return b;
}

#define BIT(v, m, gp) (((v) & (m)) ? (gp) : 0u)

// DualShock 4, report id 1 stripped: LX LY RX RY, hat|face, buttons, PS
static uint32_t decode_ds4(uint8_t const *r, uint16_t len) {
    if (len < 7) return 0;
    return gp_dirs(r[0], r[1], r[4] & 15) | BIT(r[4], 0x10, GP_WEST) | BIT(r[4], 0x20, GP_SOUTH) |
           BIT(r[4], 0x40, GP_EAST) | BIT(r[4], 0x80, GP_NORTH) | BIT(r[5], 0x01, GP_L1) |
           BIT(r[5], 0x02, GP_R1) | BIT(r[5], 0x04, GP_L2) | BIT(r[5], 0x08, GP_R2) |
           BIT(r[5], 0x10, GP_SELECT) | BIT(r[5], 0x20, GP_START) | BIT(r[5], 0x40, GP_L3) |
           BIT(r[5], 0x80, GP_R3) | BIT(r[6], 0x01, GP_HOME);
}

// DualSense, report id 1 stripped: face and hat in byte 7, buttons in 8, PS in 9
static uint32_t decode_ds5(uint8_t const *r, uint16_t len) {
    if (len < 10) return 0;
    return gp_dirs(r[0], r[1], r[7] & 15) | BIT(r[7], 0x10, GP_WEST) | BIT(r[7], 0x20, GP_SOUTH) |
           BIT(r[7], 0x40, GP_EAST) | BIT(r[7], 0x80, GP_NORTH) | BIT(r[8], 0x01, GP_L1) |
           BIT(r[8], 0x02, GP_R1) | BIT(r[8], 0x04, GP_L2) | BIT(r[8], 0x08, GP_R2) |
           BIT(r[8], 0x10, GP_SELECT) | BIT(r[8], 0x20, GP_START) | BIT(r[8], 0x40, GP_L3) |
           BIT(r[8], 0x80, GP_R3) | BIT(r[9], 0x01, GP_HOME);
}

// Logitech F710 in D mode (and the DragonRise layout of generic pads):
// LX LY RX RY, hat | X A B Y, LB RB LT RT BACK START L3 R3
static uint32_t decode_dinput(uint8_t const *r, uint16_t len, int hat) {
    if (len < 7) return 0;
    return gp_dirs(r[0], r[1], hat) | BIT(r[4], 0x10, GP_WEST) | BIT(r[4], 0x20, GP_SOUTH) |
           BIT(r[4], 0x40, GP_EAST) | BIT(r[4], 0x80, GP_NORTH) | BIT(r[5], 0x01, GP_L1) |
           BIT(r[5], 0x02, GP_R1) | BIT(r[5], 0x04, GP_L2) | BIT(r[5], 0x08, GP_R2) |
           BIT(r[5], 0x10, GP_SELECT) | BIT(r[5], 0x20, GP_START) | BIT(r[5], 0x40, GP_L3) |
           BIT(r[5], 0x80, GP_R3);
}

// Generic HID gamepad (pico-speccy's process_hid_gamepad: DragonRise family,
// buttons one byte later than the F710's, no hat)
static uint32_t decode_generic(uint8_t const *r, uint16_t len) {
    if (len < 7) return 0;
    return gp_dirs(r[0], r[1], -1) | BIT(r[5], 0x10, GP_WEST) | BIT(r[5], 0x20, GP_SOUTH) |
           BIT(r[5], 0x40, GP_EAST) | BIT(r[5], 0x80, GP_NORTH) | BIT(r[6], 0x01, GP_L1) |
           BIT(r[6], 0x02, GP_R1) | BIT(r[6], 0x04, GP_L2) | BIT(r[6], 0x08, GP_R2) |
           BIT(r[6], 0x10, GP_SELECT) | BIT(r[6], 0x20, GP_START);
}

// 2563:0575 clones, no report id: Y B A X L R, SELECT START, hat, LX LY
static uint32_t decode_2563_0575(uint8_t const *r, uint16_t len) {
    if (len < 7) return 0;
    return gp_dirs(r[3], r[4], r[2] & 15) | BIT(r[0], 0x01, GP_WEST) | BIT(r[0], 0x02, GP_SOUTH) |
           BIT(r[0], 0x04, GP_EAST) | BIT(r[0], 0x08, GP_NORTH) | BIT(r[0], 0x10, GP_L1) |
           BIT(r[0], 0x20, GP_R1) | BIT(r[1], 0x01, GP_SELECT) | BIT(r[1], 0x02, GP_START);
}

// FEED:2320, report id 7 stripped: LX LY RX RY, hat, A B Y X L R, SELECT START
static uint32_t decode_feed_2320(uint8_t const *r, uint16_t len) {
    if (len < 7) return 0;
    return gp_dirs(r[0], r[1], r[4] & 15) | BIT(r[5], 0x01, GP_SOUTH) | BIT(r[5], 0x02, GP_EAST) |
           BIT(r[5], 0x04, GP_NORTH) | BIT(r[5], 0x08, GP_WEST) | BIT(r[5], 0x10, GP_L1) |
           BIT(r[5], 0x20, GP_R1) | BIT(r[6], 0x04, GP_SELECT) | BIT(r[6], 0x08, GP_START);
}

// 0810:0001, report id stripped: -, -, LX, LY, Y B A X, LB RB LT RT SELECT START
static uint32_t decode_0810_0001(uint8_t const *r, uint16_t len) {
    if (len < 7) return 0;
    return gp_dirs(r[2], r[3], -1) | BIT(r[4], 0x10, GP_NORTH) | BIT(r[4], 0x20, GP_EAST) |
           BIT(r[4], 0x40, GP_SOUTH) | BIT(r[4], 0x80, GP_WEST) | BIT(r[5], 0x01, GP_L1) |
           BIT(r[5], 0x02, GP_R1) | BIT(r[5], 0x04, GP_L2) | BIT(r[5], 0x08, GP_R2) |
           BIT(r[5], 0x10, GP_SELECT) | BIT(r[5], 0x20, GP_START);
}

// Vendor layouts the descriptor parser cannot be trusted with. Returns 1 if
// the report was one of them.
static int process_vendor_gamepad(uint8_t instance, uint8_t const *report, uint16_t len) {
    const uint16_t vid = hid_info[instance].vid, pid = hid_info[instance].pid;
    if (is_dualshock4(vid, pid)) {
        if (len >= 10 && report[0] == 0x01) gp_hid[instance] = decode_ds4(report + 1, len - 1);
    } else if (is_dualsense(vid, pid)) {
        if (len >= 11 && report[0] == 0x01) gp_hid[instance] = decode_ds5(report + 1, len - 1);
    } else if (is_f710_dinput(vid, pid)) {
        if (len >= 8 && report[0] == 0x01) gp_hid[instance] = decode_dinput(report + 1, len - 1, report[5] & 15);
    } else if (is_gp_2563_0575(vid, pid)) {
        gp_hid[instance] = decode_2563_0575(report, len);
    } else if (is_gp_feed_2320(vid, pid)) {
        if (len >= 8 && report[0] == 0x07) gp_hid[instance] = decode_feed_2320(report + 1, len - 1);
    } else if (is_gp_0810_0001(vid, pid)) {
        if (len >= 8 && (report[0] == 0x01 || report[0] == 0x02))
            gp_hid[instance] = decode_0810_0001(report + 1, len - 1);
    } else {
        return 0;
    }
    gp_is_pad[instance] = 1;
    return 1;
}

// A generic pad through its parsed descriptor (pico-speccy, 2026-11-04).
// False: no layout or not its report (the caller keeps the fixed-layout
// decoder). The DragonRise / SNES-style families keep that decoder: their
// button choice is proven on hardware, the descriptor numbering would move
// it. Buttons by the HID Button page: 1 south, 2 east, 3 west, 4 north,
// 5/6 L1/R1, 7/8 L2/R2, 9 select, 10 start, 11/12 L3/R3, 13 home.
static int pad_layout_feed(uint8_t instance, uint8_t const *report, uint16_t len) {
    if (instance >= CFG_TUH_HID || !pad_layout[instance].use) return 0;
    const uint16_t vid = hid_info[instance].vid;
    if (vid == 0x081F || vid == 0x0079) return 0;
    hid_pad_report_t r;
    if (!hid_pad_decode(&pad_layout[instance], report, len, &r)) return 0;
    // Android-order pads (SDL gamecontrollerdb, every 20BC:5500 entry: Betop
    // BFM, ShanWan, GameSir G3w): A b0, B b1, X b3, Y b4, LB b6, RB b7, LT b8,
    // RT b9, Back b10, Start b11 (b2/b5 unused)
    if (vid == 0x20BC && hid_info[instance].pid == 0x5500) {
        static const uint8_t src[10] = {0, 1, 3, 4, 6, 7, 8, 9, 10, 11};
        uint32_t m = 0;
        for (uint8_t i = 0; i < 10; i++) m |= ((r.buttons >> src[i]) & 1u) << i;
        r.buttons = m;
    }
    static const uint32_t gp_of[13] = {GP_SOUTH, GP_EAST,   GP_WEST,  GP_NORTH, GP_L1, GP_R1, GP_L2,
                                       GP_R2,    GP_SELECT, GP_START, GP_L3,    GP_R3, GP_HOME};
    uint32_t b = (r.up ? GP_UP : 0) | (r.down ? GP_DOWN : 0) | (r.left ? GP_LEFT : 0) | (r.right ? GP_RIGHT : 0);
    for (int i = 0; i < 13; i++)
        if (r.buttons & (1u << i)) b |= gp_of[i];
    gp_hid[instance] = b;
    gp_is_pad[instance] = 1;
    return 1;
}

//--------------------------------------------------------------------
// XInput (Xbox 360 / One / original; driver from pico-speccy)
//--------------------------------------------------------------------

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)len;
    xinputh_interface_t const *itf = (xinputh_interface_t const *)report;
    const xinput_gamepad_t *p = &itf->pad;
    const uint16_t w = p->wButtons;
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    // the F710 in X mode (046D:C21F) reports garbage in the trigger bytes
    const int triggers = !(vid == 0x046D && pid == 0xC21F);
    uint32_t b = BIT(w, XINPUT_GAMEPAD_DPAD_UP, GP_UP) | BIT(w, XINPUT_GAMEPAD_DPAD_DOWN, GP_DOWN) |
                 BIT(w, XINPUT_GAMEPAD_DPAD_LEFT, GP_LEFT) | BIT(w, XINPUT_GAMEPAD_DPAD_RIGHT, GP_RIGHT) |
                 BIT(w, XINPUT_GAMEPAD_A, GP_SOUTH) | BIT(w, XINPUT_GAMEPAD_B, GP_EAST) |
                 BIT(w, XINPUT_GAMEPAD_X, GP_WEST) | BIT(w, XINPUT_GAMEPAD_Y, GP_NORTH) |
                 BIT(w, XINPUT_GAMEPAD_LEFT_SHOULDER, GP_L1) | BIT(w, XINPUT_GAMEPAD_RIGHT_SHOULDER, GP_R1) |
                 BIT(w, XINPUT_GAMEPAD_START, GP_START) | BIT(w, XINPUT_GAMEPAD_BACK, GP_SELECT) |
                 BIT(w, XINPUT_GAMEPAD_LEFT_THUMB, GP_L3) | BIT(w, XINPUT_GAMEPAD_RIGHT_THUMB, GP_R3) |
                 BIT(w, XINPUT_GAMEPAD_GUIDE, GP_HOME);
    if (triggers && p->bLeftTrigger > 0x40) b |= GP_L2;
    if (triggers && p->bRightTrigger > 0x40) b |= GP_R2;
    // left stick, ~25% deflection
    if (p->sThumbLY > 8000) b |= GP_UP;
    if (p->sThumbLY < -8000) b |= GP_DOWN;
    if (p->sThumbLX < -8000) b |= GP_LEFT;
    if (p->sThumbLX > 8000) b |= GP_RIGHT;
    (void)instance;
    if (dev_addr <= CFG_TUH_DEVICE_MAX) gp_xinput[dev_addr] = itf->connected ? b : 0;
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *itf) {
    printf("USB XInput gamepad connected (type %d)\n", (int)itf->type);
    if (dev_addr <= CFG_TUH_DEVICE_MAX) gp_xinput_on[dev_addr] = 1;
    // a wireless 360 receiver: wait for its pad to connect
    if (itf->type == XBOX360_WIRELESS && !itf->connected) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)instance;
    printf("USB XInput gamepad disconnected\n");
    if (dev_addr <= CFG_TUH_DEVICE_MAX) gp_xinput[dev_addr] = gp_xinput_on[dev_addr] = 0;
}

//--------------------------------------------------------------------
// Process generic HID report
//--------------------------------------------------------------------

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr;
    if (process_vendor_gamepad(instance, report, len)) return;

    uint8_t const rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        // Simple report without report ID
        rpt_info = &rpt_info_arr[0];
    } else {
        // Composite report, first byte is report ID
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) {
        return;
    }

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (rpt_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
                break;
            case HID_USAGE_DESKTOP_MOUSE:
                process_mouse_report((hid_mouse_report_t const *)report);
                break;
            case HID_USAGE_DESKTOP_GAMEPAD:
            case HID_USAGE_DESKTOP_JOYSTICK:
                // the composite branch above stripped the report id; the layout wants it back
                if (rpt_info->report_id ? pad_layout_feed(instance, report - 1, (uint16_t)(len + 1))
                                        : pad_layout_feed(instance, report, len))
                    break;
                gp_hid[instance] = decode_generic(report, len);
                gp_is_pad[instance] = 1;
                break;
            default:
                break;
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Callbacks
//--------------------------------------------------------------------

// Invoked when HID device is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        printf("USB keyboard connected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 1;
        printf("USB mouse connected\n");
    }

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    hid_info[instance].vid = vid;
    hid_info[instance].pid = pid;
    gp_hid[instance] = 0;
    gp_is_pad[instance] = is_dualshock4(vid, pid) || is_dualsense(vid, pid) || is_f710_dinput(vid, pid) ||
                          is_gp_2563_0575(vid, pid) || is_gp_feed_2320(vid, pid) || is_gp_0810_0001(vid, pid);

    // Generic pads: the layout from the descriptor
    if (instance < CFG_TUH_HID) {
        if (itf_protocol == HID_ITF_PROTOCOL_NONE)
            hid_pad_parse(&pad_layout[instance], desc_report, desc_len, 64);
        else
            memset(&pad_layout[instance], 0, sizeof(pad_layout[instance]));
    }

    // Parse generic report descriptor
    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
        // long composite descriptors (DS4/DS5) trip the stock parser: retry
        // with the RIP parser
        if (hid_info[instance].report_count == 0 && desc_report && desc_len) {
            tuh_hid_report_info_plus_t rip[MAX_REPORT];
            uint8_t n = tuh_hid_parse_report_descriptor_plus(rip, MAX_REPORT, desc_report, desc_len);
            if (n > MAX_REPORT) n = MAX_REPORT;
            for (uint8_t i = 0; i < n; i++) {
                hid_info[instance].report_info[i].report_id = rip[i].report_id;
                hid_info[instance].report_info[i].usage = (uint8_t)rip[i].usage;
                hid_info[instance].report_info[i].usage_page = rip[i].usage_page;
            }
            hid_info[instance].report_count = n;
        }
        printf("USB HID device %04X:%04X (%u reports)\n", vid, pid, hid_info[instance].report_count);
    }

    // the 0810:0001 dongle goes silent in boot protocol
    if (is_gp_0810_0001(vid, pid) && itf_protocol != HID_ITF_PROTOCOL_NONE)
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);

    // Request to receive reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        // Failed to request report
    }
}

// Invoked when HID device is unmounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (instance < CFG_TUH_HID) {
        gp_hid[instance] = gp_is_pad[instance] = 0;
        memset(&pad_layout[instance], 0, sizeof(pad_layout[instance])); // the slot is reused
    }

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
        printf("USB keyboard disconnected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 0;
        printf("USB mouse disconnected\n");
    }
}

// Invoked when report is received
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    // zero-length reports are legal idle traffic on some receivers
    if (!report || !len) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }
    // 0810:0001 claims boot keyboard/mouse but sends joystick reports
    if (is_gp_0810_0001(hid_info[instance].vid, hid_info[instance].pid)) {
        process_vendor_gamepad(instance, report, len);
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
            prev_kbd_report = *(hid_keyboard_report_t const *)report;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            process_mouse_report((hid_mouse_report_t const *)report);
            break;

        default:
            process_generic_report(dev_addr, instance, report, len);
            break;
    }

    // Continue receiving reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        // Failed to request next report
    }
}

//--------------------------------------------------------------------
// Public API (defined in usbhid.h)
//--------------------------------------------------------------------

void usbhid_init(void) {
    // Initialize TinyUSB Host
    tuh_init(BOARD_TUH_RHPORT);

    // Clear state
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    memset(&prev_mouse_report, 0, sizeof(prev_mouse_report));
    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    current_buttons = 0;
    mouse_has_motion = 0;
    key_action_head = 0;
    key_action_tail = 0;

    printf("USB HID Host initialized\n");
}

void usbhid_task(void) {
    // Process USB events
    tuh_task();
}

int usbhid_keyboard_connected(void) {
    return keyboard_connected;
}

int usbhid_mouse_connected(void) {
    return mouse_connected;
}

void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state) {
    if (state) {
        memcpy(state->keycode, prev_kbd_report.keycode, 6);
        state->modifier = prev_kbd_report.modifier;
        state->has_key = (key_action_head != key_action_tail);
    }
}

void usbhid_get_mouse_state(usbhid_mouse_state_t *state) {
    if (state) {
        state->dx = cumulative_dx;
        state->dy = cumulative_dy;
        state->wheel = cumulative_wheel;
        state->buttons = current_buttons;
        state->has_motion = mouse_has_motion;

        // Reset accumulated values
        cumulative_dx = 0;
        cumulative_dy = 0;
        cumulative_wheel = 0;
        mouse_has_motion = 0;
    }
}

uint32_t usbhid_gamepad_buttons(void) {
    uint32_t b = 0;
    for (int i = 0; i < CFG_TUH_HID; i++) b |= gp_hid[i];
    for (int i = 0; i <= CFG_TUH_DEVICE_MAX; i++) b |= gp_xinput[i];
    return b;
}

int usbhid_gamepad_connected(void) {
    for (int i = 0; i < CFG_TUH_HID; i++)
        if (gp_is_pad[i]) return 1;
    for (int i = 0; i <= CFG_TUH_DEVICE_MAX; i++)
        if (gp_xinput_on[i]) return 1;
    return 0;
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail) {
        return 0; // No actions queued
    }

    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

#endif // CFG_TUH_ENABLED
