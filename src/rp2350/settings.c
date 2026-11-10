/*
 * frank-castle — input (USB keyboard and gamepads) and the settings menu.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every PS1 pad button has up to two keyboard keys and one gamepad button.
 * The settings window (F12, a gamepad's Home button or Select+Start, also
 * on the welcome screen) sets the volume, the sound output, the
 * reduced-effects mode and the bindings; it pauses the game, sound
 * included, and saves what changed to /castle/castle.cfg when it closes.
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "castle_audio.h"
#include "fbcon.h"
#include "platform.h"
#include "shell.h"
#include "ui_gfx.h"
#include "usbhid.h"

extern void Platform_SetFxMode(int mode);
extern int Platform_GetFxMode(void);
extern unsigned Platform_UartPadBits(void);

#ifndef CASTLE_VERSION
#define CASTLE_VERSION "?"
#endif

#define CFG_PATH "/castle/castle.cfg"

/* ---- PS1 buttons and their defaults ---- */

typedef struct {
    const char *name; /* in the menu */
    const char *id;   /* in castle.cfg */
    uint8_t bit;      /* PS1 pad bit */
    uint8_t key[2];   /* default HID keys */
    uint8_t gp;       /* default gamepad button (GP_* bit index) */
} button_t;

#define GPI(m) ((uint8_t)__builtin_ctz(m))

static const button_t buttons[] = {
    {"Up", "up", 12, {0x52, 0}, GPI(GP_UP)},
    {"Down", "down", 14, {0x51, 0}, GPI(GP_DOWN)},
    {"Left", "left", 15, {0x50, 0}, GPI(GP_LEFT)},
    {"Right", "right", 13, {0x4F, 0}, GPI(GP_RIGHT)},
    {"Cross", "cross", 6, {0x1B, 0}, GPI(GP_SOUTH)},     /* X */
    {"Square", "square", 7, {0x1D, 0}, GPI(GP_WEST)},    /* Z */
    {"Circle", "circle", 5, {0x06, 0}, GPI(GP_EAST)},    /* C */
    {"Triangle", "triangle", 4, {0x16, 0}, GPI(GP_NORTH)}, /* S */
    {"L1", "l1", 2, {0x04, 0}, GPI(GP_L1)},             /* A */
    {"R1", "r1", 3, {0x07, 0}, GPI(GP_R1)},             /* D */
    {"L2", "l2", 0, {0x14, 0}, GPI(GP_L2)},             /* Q */
    {"R2", "r2", 1, {0x08, 0}, GPI(GP_R2)},             /* E */
    {"Start", "start", 11, {0x28, 0}, GPI(GP_START)},   /* Enter */
    {"Select", "select", 8, {0x2B, 0x2C}, GPI(GP_SELECT)}, /* Tab, Space */
};
#define NBTN ((int)(sizeof(buttons) / sizeof(buttons[0])))
#define GP_NONE 0xFF

static struct {
    uint8_t volume; /* 0..10 */
    uint8_t fx;     /* 0 full, 1 reduced when late, 2 reduced */
    uint8_t audio;  /* AUDIO_OUT_HDMI / AUDIO_OUT_I2S */
    uint8_t key[NBTN][2];
    uint8_t gp[NBTN];
} cfg;

static void defaults_keys(void) {
    for (int i = 0; i < NBTN; i++) memcpy(cfg.key[i], buttons[i].key, 2);
}

static void defaults_pad(void) {
    for (int i = 0; i < NBTN; i++) cfg.gp[i] = buttons[i].gp;
}

static void apply(void) {
    /* roughly even loudness steps */
    static const uint16_t gain[11] = {0, 3, 10, 23, 41, 64, 92, 125, 164, 207, 256};
    Audio_SetGain(gain[cfg.volume > 10 ? 10 : cfg.volume]);
    Platform_SetFxMode(cfg.fx);
    Audio_SetOutput(cfg.audio);
}

static bool cfg_ready;

static void cfg_init(void) {
    if (cfg_ready) return;
    cfg_ready = true;
    cfg.volume = 10;
    cfg.fx = (uint8_t)Platform_GetFxMode();
    cfg.audio = AUDIO_OUT_HDMI;
    defaults_keys();
    defaults_pad();
}

void Settings_Load(void) {
    cfg_init();
    FIL f;
    char line[64];
    if (f_open(&f, CFG_PATH, FA_READ) == FR_OK) {
        while (f_gets(line, sizeof(line), &f)) {
            char name[16];
            unsigned a, b;
            int v;
            if (sscanf(line, "volume=%d", &v) == 1 && v >= 0 && v <= 10) cfg.volume = (uint8_t)v;
            if (sscanf(line, "effects=%d", &v) == 1 && v >= 0 && v <= 2) cfg.fx = (uint8_t)v;
            if (!strncmp(line, "audio=", 6)) cfg.audio = !strncmp(line + 6, "i2s", 3) ? AUDIO_OUT_I2S : AUDIO_OUT_HDMI;
            if (sscanf(line, "key_%15[a-z0-9]=%x,%x", name, &a, &b) == 3)
                for (int i = 0; i < NBTN; i++)
                    if (!strcmp(name, buttons[i].id)) {
                        cfg.key[i][0] = (uint8_t)a;
                        cfg.key[i][1] = (uint8_t)b;
                    }
            if (sscanf(line, "pad_%15[a-z0-9]=%d", name, &v) == 2)
                for (int i = 0; i < NBTN; i++)
                    if (!strcmp(name, buttons[i].id)) cfg.gp[i] = (v >= 0 && v < GP_COUNT) ? (uint8_t)v : GP_NONE;
        }
        f_close(&f);
        printf("settings: " CFG_PATH " read\n");
    }
    apply();
}

static bool settings_save(void) {
    static char buf[1024] __attribute__((section(".psram_bss.settings")));
    int n = snprintf(buf, sizeof(buf), "# frank-castle settings (F12 in the game)\nvolume=%u\neffects=%u\naudio=%s\n",
                     cfg.volume, cfg.fx, cfg.audio == AUDIO_OUT_I2S ? "i2s" : "hdmi");
    for (int i = 0; i < NBTN; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "key_%s=%02x,%02x\n", buttons[i].id, cfg.key[i][0],
                      cfg.key[i][1]);
    for (int i = 0; i < NBTN; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "pad_%s=%d\n", buttons[i].id,
                      cfg.gp[i] == GP_NONE ? -1 : cfg.gp[i]);
    FIL f;
    UINT put = 0;
    if (f_open(&f, CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    FRESULT r = f_write(&f, buf, (UINT)n, &put);
    f_close(&f);
    printf("settings: saved (%d)\n", r);
    return r == FR_OK && put == (UINT)n;
}

/* ---- Input ---- */

static uint8_t key_down[256 / 8];
static uint32_t gp_now, gp_prev;
static int key_pressed = -1;   /* last key that went down (for binding) */
static bool menu_request;
static bool skip_request;      /* F9 (debug): skip the opening */
static bool swallow;           /* after the menu: nothing until all is released */

#define KEY_HELD(k) (key_down[(k) >> 3] & (1u << ((k) & 7)))

void Settings_Request(void) { menu_request = true; }

void Input_Poll(void) {
    uint8_t code;
    int down;
    while (usbhid_get_key_action(&code, &down)) {
        if (down) {
            key_down[code >> 3] |= (uint8_t)(1u << (code & 7));
            key_pressed = code;
            if (code == 0x45) menu_request = true; /* F12 */
            if (code == 0x42) skip_request = true; /* F9 */
        } else {
            key_down[code >> 3] &= (uint8_t)~(1u << (code & 7));
        }
    }
    gp_prev = gp_now;
    gp_now = usbhid_gamepad_buttons();
    const uint32_t chord = GP_START | GP_SELECT;
    if ((gp_now & GP_HOME & ~gp_prev) || ((gp_now & chord) == chord && (gp_prev & chord) != chord))
        menu_request = true;
}

bool Input_MenuRequested(void) {
    bool r = menu_request;
    menu_request = false;
    return r;
}

uint32_t Input_PadBits(void) {
    cfg_init();
    uint32_t bits = 0;
    bool any = gp_now != 0;
    for (int i = 0; i < NBTN; i++) {
        for (int k = 0; k < 2; k++)
            if (cfg.key[i][k] && KEY_HELD(cfg.key[i][k])) bits |= 1u << buttons[i].bit;
        if (cfg.gp[i] != GP_NONE && (gp_now & (1u << cfg.gp[i]))) bits |= 1u << buttons[i].bit;
    }
    for (int i = 0; i < (int)sizeof(key_down) && !any; i++) any = key_down[i] != 0;
    if (swallow) {
        if (any) return 0;
        swallow = false;
    }
    return bits;
}

/* ---- Names ---- */

static const char *key_name(uint8_t k, char *buf) {
    static const char *const named[] = {
        [0x28] = "Enter", [0x29] = "Esc", [0x2A] = "Bksp", [0x2B] = "Tab", [0x2C] = "Space",
        [0x2D] = "-", [0x2E] = "=", [0x2F] = "[", [0x30] = "]", [0x31] = "\\", [0x33] = ";",
        [0x34] = "'", [0x35] = "`", [0x36] = ",", [0x37] = ".", [0x38] = "/", [0x39] = "Caps",
        [0x49] = "Ins", [0x4A] = "Home", [0x4B] = "PgUp", [0x4C] = "Del", [0x4D] = "End",
        [0x4E] = "PgDn", [0x4F] = "Right", [0x50] = "Left", [0x51] = "Down", [0x52] = "Up",
        [0x58] = "KpEnter",
    };
    if (!k) return "-";
    if (k >= 0x04 && k <= 0x1D) {
        buf[0] = (char)('A' + k - 0x04);
        buf[1] = 0;
    } else if (k >= 0x1E && k <= 0x27) {
        buf[0] = (char)(k == 0x27 ? '0' : '1' + k - 0x1E);
        buf[1] = 0;
    } else if (k >= 0x3A && k <= 0x45) {
        snprintf(buf, 8, "F%d", k - 0x3A + 1);
    } else if (k >= 0x59 && k <= 0x62) {
        snprintf(buf, 8, "Kp%d", k == 0x62 ? 0 : k - 0x58);
    } else if (k == 0xE0) {
        return "Ctrl";
    } else if (k == 0xE1) {
        return "Shift";
    } else if (k == 0xE2) {
        return "Alt";
    } else if (k < sizeof(named) / sizeof(named[0]) && named[k]) {
        return named[k];
    } else {
        snprintf(buf, 8, "#%02X", k);
    }
    return buf;
}

static const char *gp_name(uint8_t i) {
    static const char *const names[GP_COUNT] = {
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "A / Cross", "B / Circle",
        "X / Square", "Y / Triangle", "L1 / LB", "R1 / RB", "L2 / LT", "R2 / RT",
        "Start", "Select / Back", "L3", "R3", "Home",
    };
    return i < GP_COUNT ? names[i] : "-";
}

/* ---- The menu ---- */

enum {
    NAV_UP = 1, NAV_DOWN = 2, NAV_LEFT = 4, NAV_RIGHT = 8, NAV_OK = 16, NAV_BACK = 32, NAV_DEL = 64,
    NAV_MENU = 128, NAV_PGUP = 256, NAV_PGDN = 512, NAV_HOME = 1024, NAV_END = 2048
};

/* Fixed menu keys, whatever the bindings: arrows, Enter, Esc, Del, F12,
 * PgUp/PgDn/Home/End; on a gamepad the d-pad, A (south) and B (east), Y
 * (north), LB/RB (a page), Home; on the debug UART the pad letters (cross =
 * OK, circle = back). */
static unsigned nav_held(void) {
    unsigned n = 0;
    if (KEY_HELD(0x52)) n |= NAV_UP;
    if (KEY_HELD(0x51)) n |= NAV_DOWN;
    if (KEY_HELD(0x50)) n |= NAV_LEFT;
    if (KEY_HELD(0x4F)) n |= NAV_RIGHT;
    if (KEY_HELD(0x28) || KEY_HELD(0x58)) n |= NAV_OK;
    if (KEY_HELD(0x29) || KEY_HELD(0x2A)) n |= NAV_BACK;
    if (KEY_HELD(0x4C)) n |= NAV_DEL;
    if (KEY_HELD(0x45)) n |= NAV_MENU;
    if (KEY_HELD(0x4B)) n |= NAV_PGUP;
    if (KEY_HELD(0x4E)) n |= NAV_PGDN;
    if (KEY_HELD(0x4A)) n |= NAV_HOME;
    if (KEY_HELD(0x4D)) n |= NAV_END;
    if (gp_now & GP_UP) n |= NAV_UP;
    if (gp_now & GP_DOWN) n |= NAV_DOWN;
    if (gp_now & GP_LEFT) n |= NAV_LEFT;
    if (gp_now & GP_RIGHT) n |= NAV_RIGHT;
    if (gp_now & GP_SOUTH) n |= NAV_OK;
    if (gp_now & GP_EAST) n |= NAV_BACK;
    if (gp_now & GP_NORTH) n |= NAV_DEL;
    if (gp_now & GP_L1) n |= NAV_PGUP;
    if (gp_now & GP_R1) n |= NAV_PGDN;
    if (gp_now & GP_HOME) n |= NAV_MENU;
    const unsigned u = Platform_UartPadBits();
    if (u & (1u << 12)) n |= NAV_UP;
    if (u & (1u << 14)) n |= NAV_DOWN;
    if (u & (1u << 15)) n |= NAV_LEFT;
    if (u & (1u << 13)) n |= NAV_RIGHT;
    if (u & (1u << 6)) n |= NAV_OK;
    if (u & (1u << 5)) n |= NAV_BACK;
    if (u & (1u << 4)) n |= NAV_DEL;
    if (u & (1u << 11)) n |= NAV_MENU;
    return n;
}

/* Presses since the last call; held directions repeat. */
static unsigned nav_prev;
static uint32_t nav_repeat_at;

static unsigned nav_events(void) {
    const unsigned held = nav_held();
    unsigned ev = held & ~nav_prev;
    const unsigned rep = NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT | NAV_PGUP | NAV_PGDN;
    const uint32_t now = time_us_32();
    if (ev & rep) {
        nav_repeat_at = now + 400000;
    } else if ((held & rep) && (int32_t)(now - nav_repeat_at) >= 0) {
        ev |= held & rep;
        nav_repeat_at = now + 90000;
    }
    nav_prev = held;
    return ev;
}

static void poll_all(void) {
    platform_usb_poll();
    Input_Poll();
}

/* ---- The settings window: murmheretic's F12 window ----
 *
 * frank-amiga's prefs editor as murmheretic has it: a listview of the
 * settings on the left, the selected one's gadget in a group frame on the
 * right, a breadcrumb above, a status field below, an EasyRequest-style
 * confirmation; in the colours of SOTN's own menus (ui_gfx.h), over the
 * frozen, dimmed game picture. Every change is drawn into the hidden
 * buffer and shown at vblank, so nothing flickers. */

enum { K_SUB, K_INT, K_RADIO, K_ACTION, K_KEY, K_PAD };

typedef struct node {
    const char *label;
    uint8_t kind;
    int8_t lo, hi;
    bool pct;                     /* K_INT: shown as a percentage (x10) */
    const char *const *opts;      /* K_RADIO: one per value lo..hi */
    const struct node *kids;      /* K_SUB */
    int nkids;
    int (*get)(void);
    void (*set)(int v);
    void (*run)(void);            /* K_ACTION */
    const char *(*label_fn)(void);
    int8_t idx;                   /* K_KEY / K_PAD: the PS1 button */
    const char *help;
} node_t;

static struct {
    bool in_game;
    bool close;
    const struct node *capturing; /* a K_KEY / K_PAD waiting for its key */
    char status[48];
} M;

static void status(const char *s) { snprintf(M.status, sizeof(M.status), "%s", s); }

static const char *const fx_names[3] = {"Full", "Reduced when slow", "Always reduced"};
static const char *const out_names[2] = {"HDMI", "I2S"};

static int get_volume(void) { return cfg.volume; }
static void set_volume(int v) {
    cfg.volume = (uint8_t)v;
    apply();
}
static int get_output(void) { return cfg.audio; }
static void set_output(int v) {
    cfg.audio = (uint8_t)v;
    apply();
}
static int get_fx(void) { return cfg.fx; }
static void set_fx(int v) {
    cfg.fx = (uint8_t)v;
    apply();
}

static void do_keys_default(void) {
    defaults_keys();
    status("Keyboard: defaults restored");
}
static void do_pad_default(void) {
    defaults_pad();
    status("Gamepad: defaults restored");
}
static void do_resume(void) { M.close = true; }

static bool confirm(const char *title, const char *text);
static bool cfg_changed(void);
static void do_restart(void) {
    if (!confirm("Restart", "Restart the console?")) return;
    if (cfg_changed()) settings_save();
    platform_reboot(false);
}
static const char *resume_label(void) { return M.in_game ? "Back to the game" : "Start the game"; }

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const node_t audio_kids[] = {
    {"Volume", K_INT, 0, 10, .pct = true, .get = get_volume, .set = set_volume,
     .help = "Everything the game\nplays: music, effects,\nvoices.\nLeft/Right to adjust."},
    {"Sound output", K_RADIO, 0, 1, .opts = out_names, .get = get_output, .set = set_output,
     .help = "HDMI: the TV, in the\nHDMI signal (44.1 kHz).\nI2S: the DAC module.\nAt once."},
};

static const node_t display_kids[] = {
    {"Effects", K_RADIO, 0, 2, .opts = fx_names, .get = get_fx, .set = set_fx,
     .help = "Big see-through effects\n(light, fog, fades)\ndraw every other line\npair: smoother where\nthe game would slow."},
};

/* the bindings pages: the PS1 buttons, then "Restore defaults" */
static node_t kb_kids[NBTN + 1] __attribute__((section(".psram_bss.settings")));
static node_t pad_kids[NBTN + 1] __attribute__((section(".psram_bss.settings")));

static const node_t root_kids[] = {
    {"Audio", K_SUB, .kids = audio_kids, .nkids = COUNT(audio_kids)},
    {"Display", K_SUB, .kids = display_kids, .nkids = COUNT(display_kids)},
    {"Keyboard", K_SUB, .kids = kb_kids, .nkids = NBTN + 1},
    {"Gamepad", K_SUB, .kids = pad_kids, .nkids = NBTN + 1},
    {NULL, K_ACTION, .run = do_resume, .label_fn = resume_label,
     .help = "Close this window.\nWhat changed is saved\nto /castle/castle.cfg."},
    {"Restart", K_ACTION, .run = do_restart,
     .help = "Save what changed and\nrestart the console\n(asks first)."},
};

static const node_t root = {"Settings", K_SUB, .kids = root_kids, .nkids = COUNT(root_kids)};

static void build_bindings(void) {
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < NBTN; i++) {
        kb_kids[i] = (node_t){buttons[i].name, K_KEY, .idx = (int8_t)i,
                              .help = "Enter, then the new\nkey. Esc cancels.\nDel: this one's\ndefault."};
        pad_kids[i] = (node_t){buttons[i].name, K_PAD, .idx = (int8_t)i,
                               .help = "Enter, then the new\nbutton. Esc or Home\ncancels. Y or Del:\nthis one's default."};
    }
    kb_kids[NBTN] = (node_t){"Restore defaults", K_ACTION, .run = do_keys_default,
                             .help = "Back to the keys this\nport started with."};
    pad_kids[NBTN] = (node_t){"Restore defaults", K_ACTION, .run = do_pad_default,
                              .help = "Back to the buttons by\nposition: A / cross is\nthe bottom one."};
}

static const char *node_label(const node_t *n) { return n->label_fn ? n->label_fn() : n->label; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int node_get(const node_t *n) { return clampi(n->get(), n->lo, n->hi); }

/* a binding's value: "X", "Tab / Space", "A / Cross" */
static const char *bind_value(const node_t *n, char *buf) {
    char a[8], b[8];
    const int i = n->idx;
    if (n->kind == K_PAD) return cfg.gp[i] == GP_NONE ? "-" : gp_name(cfg.gp[i]);
    if (cfg.key[i][1])
        snprintf(buf, 24, "%s / %s", key_name(cfg.key[i][0], a), key_name(cfg.key[i][1], b));
    else
        snprintf(buf, 24, "%s", key_name(cfg.key[i][0], a));
    return buf;
}

static const char *value_text(const node_t *n, char *buf) {
    switch (n->kind) {
    case K_INT:
        snprintf(buf, 24, n->pct ? "%d%%" : "%d", node_get(n) * (n->pct ? 10 : 1));
        return buf;
    case K_RADIO: return n->opts[node_get(n) - n->lo];
    case K_KEY:
    case K_PAD: return bind_value(n, buf);
    case K_SUB: return SYM_CHEV;
    default: return NULL;
    }
}

/* ---- navigation state ---- */

#define MAX_DEPTH 3
static const node_t *stack[MAX_DEPTH];
static int sel_stack[MAX_DEPTH], top_stack[MAX_DEPTH];
static int depth;

/* ---- layout (640x240 hires: murmheretic's, its x doubled) ---- */

#define WX0 16
#define WY0 8
#define WX1 624
#define WY1 232
#define CRUMB_Y (WY0 + 25)
#define LX0 28
#define LX1 300
#define LY0 48
#define LY1 204
#define RX0 312
#define RX1 612
#define ROW_H 11
#define SY0 209
#define SY1 225
#define LIST_ROWS ((LY1 - LY0 - 4) / ROW_H)

static uint16_t backdrop[UI_W * UI_H] __attribute__((section(".psram_bss.settings"), aligned(4)));

/* ---- drawing ---- */

static void help_text(int x, int y, const char *s) {
    char line[48];
    while (s && *s) {
        const char *e = strchr(s, '\n');
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        snprintf(line, sizeof(line), "%.*s", (int)n, s);
        if (y + UI_FONT_H > LY1 - 4) break; /* the group frame's bottom */
        ui_text(x, y, line, C_TEXT_DIM);
        y += ROW_H;
        s = e ? e + 1 : NULL;
    }
}

static void draw_right(const node_t *n) {
    const int x0 = RX0, y0 = LY0 + 4, x1 = RX1, y1 = LY1;
    char buf[32];
    ui_group(x0, y0, x1 - x0, y1 - y0, node_label(n));
    const int ix = x0 + 18;
    int y = y0 + 12;

    switch (n->kind) {
    case K_INT: {
        const int tx0 = ix, tx1 = x1 - 18, ty = y + 4, kw = 24;
        ui_well(tx0, ty, tx1 - tx0, 11, C_WELL);
        const int span = tx1 - tx0 - 4 - kw;
        const int v = node_get(n);
        const int kx = tx0 + 2 + (n->hi > n->lo ? span * (v - n->lo) / (n->hi - n->lo) : 0);
        ui_gradient(tx0 + 2, ty + 1, kx - tx0 - 2, 9, BGR555(120, 16, 24), BGR555(200, 40, 40)); /* filled */
        ui_fill(kx, ty + 1, kw, 9, C_KNOB);
        ui_bevel(kx, ty + 1, kw, 9, true);
        snprintf(buf, sizeof(buf), n->pct ? "%d%%" : "%d", n->lo * (n->pct ? 10 : 1));
        ui_text(tx0, ty + 16, buf, C_TEXT_DIM);
        snprintf(buf, sizeof(buf), n->pct ? "%d%%" : "%d", n->hi * (n->pct ? 10 : 1));
        ui_text_right(tx1, ty + 16, buf, C_TEXT_DIM);
        ui_text_center(tx0, tx1, ty + 16, value_text(n, buf), C_GOLD);
        y = ty + 36;
        break;
    }
    case K_RADIO: {
        const int count = n->hi - n->lo + 1, v = node_get(n);
        for (int i = 0; i < count; i++, y += 14) {
            ui_mx(ix, y, v == n->lo + i);
            ui_text(ix + 24, y, n->opts[i], v == n->lo + i ? C_WHITE : C_TEXT);
        }
        y += 6;
        break;
    }
    case K_KEY:
    case K_PAD: {
        /* the binding as a raised keycap; recessed and red while waiting */
        const bool wait = M.capturing == n;
        const char *name = wait ? "?" : bind_value(n, buf);
        int w = ui_text_width(name) + 32;
        if (w < 80) w = 80;
        const int kx = (x0 + x1 - w) / 2;
        ui_fill(kx, y + 2, w, 19, wait ? C_SEL_BAND : C_KNOB);
        ui_bevel(kx, y + 2, w, 19, !wait);
        ui_text_center(kx, kx + w, y + 7, name, wait ? C_WHITE : C_SHADOW);
        y += 32;
        if (wait) {
            ui_text(ix, y, n->kind == K_KEY ? "Press the new key." : "Press the new button.", C_ACCENT);
            ui_text(ix, y + ROW_H, n->kind == K_KEY ? "Esc cancels." : "Esc or Home cancels.", C_TEXT_DIM);
            return;
        }
        if (n->kind == K_PAD) {
            ui_text(ix, y, usbhid_gamepad_connected() ? "Gamepad connected." : "No gamepad connected.",
                    usbhid_gamepad_connected() ? C_GOLD : C_ACCENT);
            y += ROW_H + 4;
        }
        break;
    }
    case K_SUB: {
        const int fit = (y1 - 8 - y) / ROW_H;
        for (int i = 0; i < n->nkids && i < fit; i++, y += ROW_H) {
            const bool more = i == fit - 1 && n->nkids > fit;
            ui_text(ix, y, more ? "..." : node_label(&n->kids[i]), C_TEXT);
            if (more) break;
            const char *v = value_text(&n->kids[i], buf);
            if (v && n->kids[i].kind != K_SUB) ui_text_right(x1 - 18, y, v, C_GOLD);
        }
        y += 6;
        break;
    }
    case K_ACTION: {
        const char *l = node_label(n);
        int w = ui_text_width(l) + 48;
        if (w < 112) w = 112;
        ui_button((x0 + x1 - w) / 2, y + 2, w, 17, l, true);
        y += 30;
        break;
    }
    }
    help_text(ix, y, n->help);
}

static void draw_status(const node_t *n) {
    ui_well(LX0, SY0, RX1 - LX0, SY1 - SY0, C_WELL);
    const char *h = SYM_UP SYM_DOWN " Move   " SYM_ENTER " Open   Esc Close";
    switch (n->kind) {
    case K_INT: h = SYM_LEFT SYM_RIGHT " Adjust   Esc Back"; break;
    case K_RADIO: h = SYM_LEFT SYM_RIGHT "/" SYM_ENTER " Change   Esc Back"; break;
    case K_KEY: h = M.capturing ? "Press a key   Esc Cancel" : SYM_ENTER " Change key   Del Default   Esc Back"; break;
    case K_PAD:
        h = M.capturing ? "Press a button   Esc Cancel" : SYM_ENTER " Change button   Del Default   Esc Back";
        break;
    case K_ACTION: h = depth ? SYM_ENTER " Run   Esc Back" : SYM_ENTER " Run   Esc Close"; break;
    default:
        if (depth > 0) h = SYM_UP SYM_DOWN " Move   " SYM_ENTER " Open   Esc Back";
        break;
    }
    /* a message takes the hint's place until the next key: both don't fit */
    if (M.status[0])
        ui_text_clip(LX0 + 8, SY0 + 4, RX1 - LX0 - 16, M.status, C_GOLD);
    else
        ui_text(LX0 + 8, SY0 + 4, h, C_TEXT);
}

static void draw(void) {
    const node_t *cur = stack[depth];
    const int c = cur->nkids;
    int *sel = &sel_stack[depth], *top = &top_stack[depth];
    if (*sel >= c) *sel = c - 1;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + LIST_ROWS) *top = *sel - LIST_ROWS + 1;

    uint16_t *s = shell_hires();
    extern void Video_PsramPace(void);
    for (int y = 0; y < UI_H; y++) { /* letting the scanout's own PSRAM reads through */
        Video_PsramPace();
        memcpy(s + y * UI_W, backdrop + y * UI_W, UI_W * 2);
    }
    ui_begin(s);
    ui_window(WX0, WY0, WX1, WY1, "CASTLEVANIA SETTINGS", "frank-castle " CASTLE_VERSION);

    /* the breadcrumb: the levels above dim; what is plugged in, right */
    int x = LX0;
    for (int i = 0; i <= depth; i++) {
        if (i) x += ui_text(x, CRUMB_Y, " " SYM_CHEV " ", C_TEXT_DIM);
        x += ui_text(x, CRUMB_Y, stack[i]->label, i == depth ? C_TEXT : C_TEXT_DIM);
    }
    char conn[48];
    snprintf(conn, sizeof(conn), "Keyboard %s   Gamepad %s", usbhid_keyboard_connected() ? "yes" : "no",
             usbhid_gamepad_connected() ? "yes" : "no");
    ui_text_right(RX1, CRUMB_Y, conn, C_TEXT_DIM);

    ui_well(LX0, LY0, LX1 - LX0, LY1 - LY0, C_WELL);
    char buf[32];
    for (int r = 0; r < LIST_ROWS && *top + r < c; r++) {
        const int i = *top + r;
        const node_t *n = &cur->kids[i];
        const int y = LY0 + 2 + r * ROW_H;
        const bool sl = i == *sel;
        if (sl) {
            ui_fill(LX0 + 4, y, LX1 - LX0 - 8, ROW_H, M.capturing ? C_SEL_BAND : C_SEL_BG);
            ui_hline(LX0 + 4, y, LX1 - LX0 - 8, M.capturing ? C_ACCENT : C_SEL_LT);
        }
        const char *v = value_text(n, buf);
        const int vw = v ? ui_text_width(v) : 0;
        ui_text_clip(LX0 + 8, y + 1, LX1 - LX0 - 24 - vw, node_label(n), sl ? C_WHITE : C_TEXT);
        if (v) ui_text_right(LX1 - 10, y + 1, v, sl ? C_WHITE : C_GOLD);
    }
    /* more rows than fit: a scroll bar along the list's right edge */
    if (c > LIST_ROWS) {
        const int h = LY1 - LY0 - 4;
        const int th = h * LIST_ROWS / c, ty = LY0 + 2 + (h - th) * *top / (c - LIST_ROWS);
        ui_fill(LX1 - 7, LY0 + 2, 2, h, C_SHADOW);
        ui_fill(LX1 - 7, ty, 2, th, C_GOLD);
    }
    draw_right(&cur->kids[*sel]);
    draw_status(&cur->kids[*sel]);
    shell_show();
}

/* ---- EasyRequest ---- */

static bool confirm(const char *title, const char *text) {
    const int w = 400, h = 76;
    const int x0 = (UI_W - w) / 2, y0 = (UI_H - h) / 2;
    char t[40];
    snprintf(t, sizeof(t), "Request: %s", title);
    bool yes = false;
    nav_prev = nav_held();
    for (;;) {
        ui_begin(shell_hires());
        ui_window(x0, y0, x0 + w, y0 + h, t, NULL);
        ui_text_center(x0, x0 + w, y0 + 28, text, C_TEXT);
        ui_button(x0 + 40, y0 + h - 26, 96, 17, "Yes", yes);
        ui_button(x0 + w - 136, y0 + h - 26, 96, 17, "No", !yes);
        shell_show();
        for (;;) {
            shell_tick();
            poll_all();
            const int k = key_pressed;
            key_pressed = -1;
            if (k == 0x1C) return true;  /* Y */
            if (k == 0x11) return false; /* N */
            const unsigned ev = nav_events();
            if (ev & (NAV_LEFT | NAV_RIGHT)) {
                yes = !yes;
                break;
            }
            if (ev & (NAV_BACK | NAV_MENU)) return false;
            if (ev & NAV_OK) return yes;
        }
    }
}

/* ---- the session ---- */

static void adjust(const node_t *n, int dir) {
    const int v = node_get(n);
    if (n->kind == K_INT) {
        const int nv = clampi(v + dir, n->lo, n->hi);
        if (nv != v) n->set(nv);
    } else if (n->kind == K_RADIO) {
        const int span = n->hi - n->lo + 1;
        n->set(n->lo + ((v - n->lo + dir) % span + span) % span);
    } else {
        return;
    }
    char buf[24];
    snprintf(M.status, sizeof(M.status), "%s: %s", node_label(n), value_text(n, buf));
}

static void restore_one(const node_t *n) {
    const int i = n->idx;
    if (n->kind == K_KEY)
        memcpy(cfg.key[i], buttons[i].key, 2);
    else
        cfg.gp[i] = buttons[i].gp;
    char buf[24];
    snprintf(M.status, sizeof(M.status), "%s: %s (default)", n->label, bind_value(n, buf));
}

/* one navigation event */
static void handle_nav(unsigned ev) {
    const node_t *cur = stack[depth];
    const int c = cur->nkids;
    int *sel = &sel_stack[depth];
    const node_t *n = &cur->kids[*sel];
    M.status[0] = 0;

    if (ev & NAV_MENU) {
        M.close = true;
    } else if (ev & NAV_BACK) {
        if (depth == 0)
            M.close = true;
        else
            depth--;
    } else if (ev & NAV_UP) {
        *sel = (*sel + c - 1) % c;
    } else if (ev & NAV_DOWN) {
        *sel = (*sel + 1) % c;
    } else if (ev & NAV_PGUP) {
        *sel = *sel > LIST_ROWS ? *sel - LIST_ROWS : 0;
    } else if (ev & NAV_PGDN) {
        *sel = *sel + LIST_ROWS < c ? *sel + LIST_ROWS : c - 1;
    } else if (ev & NAV_HOME) {
        *sel = 0;
    } else if (ev & NAV_END) {
        *sel = c - 1;
    } else if (ev & NAV_DEL) {
        if (n->kind == K_KEY || n->kind == K_PAD) restore_one(n);
    } else if (ev & NAV_LEFT) {
        if (n->kind == K_INT || n->kind == K_RADIO)
            adjust(n, -1);
        else if (depth > 0)
            depth--;
    } else if (ev & (NAV_RIGHT | NAV_OK)) {
        const bool ok = (ev & NAV_OK) != 0;
        if (n->kind == K_SUB && depth + 1 < MAX_DEPTH) {
            stack[++depth] = n;
            sel_stack[depth] = top_stack[depth] = 0;
        } else if (n->kind == K_ACTION) {
            if (ok) n->run();
        } else if (n->kind == K_KEY || n->kind == K_PAD) {
            if (ok) M.capturing = n;
        } else if (n->kind == K_RADIO || !ok) {
            adjust(n, +1);
        }
    }
}

/* a letter jumps to the next row starting with it */
static void letter_jump(int key) {
    const node_t *cur = stack[depth];
    const int c = cur->nkids;
    int *sel = &sel_stack[depth];
    const char k = (char)('a' + key - 0x04);
    for (int i = 1; i <= c; i++) {
        const int j = (*sel + i) % c;
        char f = node_label(&cur->kids[j])[0];
        if (f >= 'A' && f <= 'Z') f = (char)(f - 'A' + 'a');
        if (f == k) {
            *sel = j;
            M.status[0] = 0;
            return;
        }
    }
}

static __typeof__(cfg) cfg_before;
static bool cfg_changed(void) { return memcmp(&cfg_before, &cfg, sizeof(cfg)) != 0; }

static bool menu_open;

void Settings_Run(bool in_game) {
    if (menu_open) return;
    menu_open = true;
    cfg_init();
    build_bindings();
    if (in_game) Audio_Pause(true);
    const bool was_shown = shell_active;
    cfg_before = cfg;
    memset(&M, 0, sizeof(M));
    M.in_game = in_game;
    depth = 0;
    stack[0] = &root;
    sel_stack[0] = top_stack[0] = 0;
    extern void Video_GrabFrame(uint16_t *dst);
    Video_GrabFrame(backdrop); /* the game (or the welcome screen) as it stands */
    uint32_t capture_at = 0, gp_cap_prev = 0, last_draw = time_us_32();
    int last_conn = usbhid_keyboard_connected() | usbhid_gamepad_connected() << 1;
    bool redraw = true;
    printf("settings: menu open\n");
    poll_all();
    nav_prev = nav_held(); /* the key that opened the menu does nothing here */
    key_pressed = -1;
    while (!M.close) {
        if (redraw) {
            draw();
            last_draw = time_us_32();
            redraw = false;
        }
        shell_tick();
        poll_all();
        if (M.capturing) {
            /* the next key or gamepad button; Esc (or 6 s) cancels */
            const node_t *n = M.capturing;
            if (!capture_at) {
                capture_at = time_us_32();
                key_pressed = -1;
                gp_cap_prev = gp_now;
            }
            const uint32_t gp_new = gp_now & ~gp_cap_prev;
            gp_cap_prev = gp_now;
            const int key = key_pressed;
            key_pressed = -1;
            const int i = n->idx;
            bool end = time_us_32() - capture_at > 6000000u, bound = false;
            if (n->kind == K_KEY && key >= 0) {
                if (key != 0x29 && key != 0x45) {
                    cfg.key[i][0] = (uint8_t)key;
                    cfg.key[i][1] = 0;
                    bound = true;
                }
                end = true;
            } else if (n->kind == K_PAD && gp_new && !(gp_new & GP_HOME)) {
                cfg.gp[i] = (uint8_t)__builtin_ctz(gp_new);
                bound = end = true;
            } else if (n->kind == K_PAD && (key == 0x29 || (gp_new & GP_HOME))) {
                end = true;
            }
            if (end) {
                char buf[24];
                if (bound)
                    snprintf(M.status, sizeof(M.status), "%s: %s", n->label, bind_value(n, buf));
                else
                    status("Cancelled");
                M.capturing = NULL;
                capture_at = 0;
                redraw = true;
                nav_prev = nav_held(); /* the bound key is not also a menu press */
            }
            continue;
        }
        const int key = key_pressed;
        key_pressed = -1;
        const unsigned ev = nav_events();
        if (ev) {
            redraw = true;
            handle_nav(ev);
        } else if (key >= 0x04 && key <= 0x1D) {
            redraw = true;
            letter_jump(key);
        } else if (time_us_32() - last_draw > 500000u) {
            /* plugging in shows in the breadcrumb row: only then redraw */
            const int conn = usbhid_keyboard_connected() | usbhid_gamepad_connected() << 1;
            if (conn != last_conn) redraw = true;
            last_conn = conn;
            last_draw = time_us_32();
        }
    }
    if (cfg_changed()) settings_save();
    printf("settings: menu closed\n");
    swallow = true;
    menu_request = false;
    if (in_game) {
        shell_release();
        Audio_Pause(false);
    } else if (was_shown) {
        shell_welcome();
    }
    menu_open = false;
}

/* ---- F9: skip the opening (debug) ----
 * The title, the file menus and Richter's prologue take minutes to get
 * through: F9 there restarts the console straight into the Castle Entrance
 * (--stage no3, Alucard's start), as the bench's "~ARGS --stage no3~" does.
 * The arguments survive the reboot in uninitialised SRAM. */
#define SKIP_MAGIC 0x5c1b0f09u
static volatile uint32_t __attribute__((section(".uninitialized_data"))) skip_boot;
static const char skip_args[] = "--stage no3";

bool Settings_SkipRequested(void) {
    bool r = skip_request;
    skip_request = false;
    return r;
}

const char *Settings_BootArgs(void) {
    if (skip_boot != SKIP_MAGIC) return NULL;
    skip_boot = 0;
    return skip_args;
}

/* F9 on the welcome screen: the arguments for this very boot */
void Settings_SkipAtBoot(void) { skip_boot = SKIP_MAGIC; }

static void debug_skip(void) {
    extern int g_GameState, g_StageId; /* GameState, Stages: int-sized (-fno-short-enums) */
    enum { GAME_PLAY = 2, STAGE_ST0 = 0x1F };
    if (g_GameState == GAME_PLAY && g_StageId != STAGE_ST0) {
        printf("F9: nothing to skip here (stage 0x%02x)\n", g_StageId);
        return;
    }
    printf("F9: restarting into the Castle Entrance\n");
    skip_boot = SKIP_MAGIC;
    platform_reboot(false);
}

void Settings_Frame(void) {
    extern void Video_HoldBlack(bool on);
    Video_HoldBlack(false); /* the game's frame is complete (shell_handover) */
    Input_Poll();
    if (Settings_SkipRequested()) debug_skip();
    if (Input_MenuRequested()) {
        Settings_Run(true);
    } else if (shell_active) {
        /* the game's first frame: the welcome screen goes */
        shell_release();
    }
}
