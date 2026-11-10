/*
 * frank-castle — Castlevania: Symphony of the Night port for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 *
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

/*
 * Game firmware entry: board bring-up, HDMI scanout on core 1, SD card and
 * disc image, then the decomp's own Main() (src/pc/main.c, built as
 * sotn_main). Extra command-line arguments, e.g. "--stage nz0", can be put
 * in /castle/args.txt.
 */

#include "board_config.h"
#include "platform.h"
#include "psx_video.h"
#include "fbcon.h"
#include "cd_fatfs.h"
#include "disc_provider.h"
#include "ff.h"
#include "vram_pitch.h"
#include "diskio.h"
#include "shell.h"
#include "settings.h"
#include "usbhid.h"
#include "castle_audio.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sysinfo.h"

#include <stdio.h>
#include <string.h>

extern int sotn_main(int argc, char *argv[]);
extern bool InitPlatform(void);
extern unsigned short g_PsxVram[];

extern void PsxVideo_Core1Loop(void);

/* SOTN draws a frame, calls VSync and only then flips to it, so at vblank
 * psyz's display area still points at the buffer about to be redrawn. Its
 * g_CurrentBuffer->disp is always the last completed frame. */
#include <game.h>
bool Psyz_DisplayOrigin(int *x, int *y) {
    GpuBuffer *b = g_CurrentBuffer;
    if (!b || b->disp.disp.w == 0) return false;
    *x = b->disp.disp.x;
    *y = b->disp.disp.y;
    return true;
}


static void core1_main(void) {
    platform_cyccnt_enable(); /* core 1's own DWT, for cycle diagnostics */
    psxv_start();
#if defined(CASTLE_PROFILE) && CASTLE_PROFILE == 2
    extern void profiler_start(void);
    profiler_start(); /* samples core 1: the IRQ lives on the core that enables it */
#endif
    extern void Video_StreamInit(void);
    Video_StreamInit();
    PsxVideo_Core1Loop();
}

/* Waiting on an error screen: USB, the UART (~BOOT~, ~RST~) and the drips.
 * True once a key or a gamepad button went down. */
static bool any_key(void) {
    static uint32_t gp_prev;
    int ch;
    while ((ch = getchar_timeout_us(0)) >= 0) platform_uart_cmd(ch);
    platform_usb_poll();
    uint8_t code;
    int down;
    bool hit = false;
    while (usbhid_get_key_action(&code, &down))
        if (down) hit = true;
    uint32_t gp = usbhid_gamepad_buttons();
    if (gp & ~gp_prev) hit = true;
    gp_prev = gp;
    shell_tick();
    return hit;
}

/* A fatal error: its message on the error screen until a key, then a
 * restart. */
static void __attribute__((noreturn)) fatal(const char *what, const char *detail) {
    printf("FATAL: %s: %s\n", what, detail);
    shell_error(what, "Press any key to restart");
    shell_errorf("%s\n", detail);
    while (!any_key()) {
    }
    platform_reboot(false);
}

/* The game walked into an area whose stage is not built yet (sim_pc.c). */
void Platform_StageMissing(const char *name) {
    static char msg[48];
    snprintf(msg, sizeof(msg), "The stage '%s' is not in this build.", name);
    fatal("AREA NOT AVAILABLE", msg);
}

static bool find_cue(char *out, size_t outlen) {
    FFDIR dir;
    FILINFO fi;
    if (f_opendir(&dir, "/castle") != FR_OK) return false;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if (n > 4 && (strcmp(fi.fname + n - 4, ".cue") == 0 || strcmp(fi.fname + n - 4, ".CUE") == 0)) {
            snprintf(out, outlen, "/castle/%s", fi.fname);
            f_closedir(&dir);
            return true;
        }
    }
    f_closedir(&dir);
    return false;
}

/* Split /castle/args.txt (or the CASTLE_ARGS build default) into argv
 * entries, whitespace separated. */
/* Test scripts: "~ARGS <arguments>~" on the UART within ~1.5 s of boot
 * replaces the game arguments for this run (e.g. "~ARGS --stage no0~"). */
static bool uart_args(char *buf, size_t len) {
    static const char tag[] = "~ARGS ";
    size_t pos = 0, n = 0;
    absolute_time_t end = make_timeout_time_ms(1500);
    /* the RX FIFO holds 32 bytes: senders wait for this before sending */
    printf("~ARGS?\n");
    while (!time_reached(end)) {
        int ch = getchar_timeout_us(1000);
        if (ch < 0) continue;
        if (pos < sizeof(tag) - 1) {
            pos = ch == tag[pos] ? pos + 1 : (ch == tag[0]);
            continue;
        }
        if (ch == '~') {
            buf[n] = 0;
            printf("args from UART: %s\n", buf);
            return true;
        }
        if (n + 1 < len) buf[n++] = (char)ch;
    }
    return false;
}

static int read_extra_args(char *buf, size_t len, char **argv, int max) {
    FIL f;
    UINT got = 0;
    const char *skip = Settings_BootArgs();
    if (skip) {
        /* F9 restarted the console to skip the opening */
        strncpy(buf, skip, len - 1);
        buf[len - 1] = 0;
        printf("args from F9: %s\n", buf);
    } else if (uart_args(buf, len)) {
        /* given on the UART */
    } else if (f_open(&f, "/castle/args.txt", FA_READ) == FR_OK) {
        f_read(&f, buf, (UINT)len - 1, &got);
        f_close(&f);
        buf[got] = 0;
    } else {
        strncpy(buf, CASTLE_ARGS, len - 1);
        buf[len - 1] = 0;
    }
    int n = 0;
    for (char *tok = strtok(buf, " \t\r\n"); tok && n < max; tok = strtok(NULL, " \t\r\n"))
        argv[n++] = tok;
    return n;
}

/* "~C errtest sd|disc|game~": show an error screen for 8 s, then restart
 * (screenshots of the screens without pulling the card). */
void Shell_ErrTest(const char *what) {
    while (*what == ' ') what++;
    if (!strcmp(what, "sd")) {
        shell_error("NO SD CARD", "Waiting for the card...");
        shell_printf("Insert a FAT32 or exFAT microSD card with your Castlevania: SOTN disc image in:\n");
        shell_highlight("  /castle/<name>.cue + .bin (SLUS-00067)\n");
    } else if (!strcmp(what, "disc")) {
        shell_error("NO GAME DISC", "Waiting for the files...");
        shell_printf("Copy your dump of the USA disc (SLUS-00067, BIN/CUE, MODE2/2352) to the card:\n");
        shell_highlight("  /castle/Castlevania - Symphony of the Night (USA).cue\n  /castle/... (Track 1).bin\n");
    } else {
        shell_error("AREA NOT AVAILABLE", "Press any key to restart");
        shell_errorf("The stage 'xyz' is not in this build.\n");
    }
    const uint32_t t0 = time_us_32();
    while (time_us_32() - t0 < 8000000u) shell_tick();
    platform_reboot(false);
}

static void wait_for_sd(void) {
    if (platform_mount_sd() == FR_OK) return;
    shell_error("NO SD CARD", "Waiting for the card...");
    shell_printf("Insert a FAT32 or exFAT microSD card with your Castlevania: SOTN disc image in:\n");
    shell_highlight("  /castle/<name>.cue + .bin (SLUS-00067)\n");
    for (;;) {
        const uint32_t until = time_us_32() + 1000000;
        while ((int32_t)(time_us_32() - until) < 0) any_key();
        disk_initialize(0);
        if (platform_mount_sd() == FR_OK) break;
    }
    shell_welcome();
    shell_printf("SD card mounted.\n");
}

static void wait_for_disc(char *cue, size_t len) {
    if (find_cue(cue, len)) return;
    shell_error("NO GAME DISC", "Waiting for the files...");
    shell_printf("Copy your dump of the USA disc (SLUS-00067, BIN/CUE, MODE2/2352) to the card:\n");
    shell_highlight("  /castle/Castlevania - Symphony of the Night (USA).cue\n  /castle/... (Track 1).bin\n");
    /* the card may be swapped meanwhile: mount again each time */
    for (;;) {
        const uint32_t until = time_us_32() + 2000000;
        while ((int32_t)(time_us_32() - until) < 0) any_key();
        disk_initialize(0);
        if (platform_mount_sd() == FR_OK && find_cue(cue, len)) break;
    }
    shell_welcome();
    shell_printf("Game disc found.\n");
}

int main(void) {
    platform_init_clocks();
    platform_report_previous_crash();
    platform_init_psram();

    /* Scanout reads the PS1 VRAM model; start it before anything can fail. */
    psxv_init();
    InitPlatform();
    multicore_reset_core1();
    /* core 1's default stack is 2 KB at the top of SCRATCH_X, right above
     * the scanout code there; it mixes the SPU from inside the GPU worker
     * (while it waits for core 0), which needs more */
    static uint32_t core1_stack[6 * 1024 / 4];
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof(core1_stack));

    shell_welcome();
    shell_printf("frank-castle %s on M2, RP2350%s\n", CASTLE_VERSION,
                 (*(io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET) & 1) ? "A" : "B");
    platform_init_usb();
    Audio_Init(); /* HDMI (HSTX IRQ, core 1) or I2S (its IRQ on core 0) */
#if CASTLE_USB_CONSOLE
    shell_printf("USB: the CDC console (development build).\n");
#else
    shell_printf("USB host started: keyboard, gamepad.\n");
#endif

    wait_for_sd();
    static char cue[160];
    wait_for_disc(cue, sizeof(cue));
    /* SD cards occasionally drop out right after a warm reset (or when a USB
     * device enumerates): re-initialise the card and mount again. */
    for (int attempt = 0; !DiscProvider_Mount(&cd_fatfs_backend, cue); attempt++) {
        if (attempt == 3) fatal("CANNOT READ THE DISC", cue);
        printf("disc mount failed, re-initialising the SD card (%d)\n", attempt + 1);
        sleep_ms(200 << attempt);
        disk_initialize(0);
        platform_mount_sd();
    }
    shell_printf("Disc: %s\n", cue + 8);
    Settings_Load();
    shell_dim("F12 or Select+Start: settings, also in the game.\n");
    shell_highlight("Starting Castlevania...\n");
    /* the welcome stays up 4 s, and at least 2.5 s after the hint above:
     * the settings can be opened meanwhile */
    const uint32_t hint_at = shell_shown_ms();
    while (shell_shown_ms() < 4000 || shell_shown_ms() - hint_at < 2500) {
        Input_Poll();
        int ch;
        while ((ch = getchar_timeout_us(0)) >= 0) platform_uart_cmd(ch);
        if (Input_MenuRequested()) {
            Settings_Run(false);
            shell_highlight("Starting Castlevania...\n");
            break;
        }
        if (Settings_SkipRequested()) { /* F9: straight to the Castle Entrance */
            Settings_SkipAtBoot();
            shell_highlight("F9: to the Castle Entrance.\n");
            break;
        }
        platform_usb_poll();
        shell_tick();
    }

    static char args_buf[256];
    static char *argv[16] = {"sotn", "--disk", cue};
    int argc = 3;
    argc += read_extra_args(args_buf, sizeof(args_buf), argv + argc, 16 - argc - 1);
    argv[argc] = NULL;

#if defined(CASTLE_PROFILE) && CASTLE_PROFILE == 1
    extern void profiler_start(void);
    profiler_start();
#endif
    printf("starting game:");
    for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");

    int r = sotn_main(argc, argv);
    printf("game exited (%d)\n", r);
    /* bad ~ARGS~ end up here: keep answering ~BOOT~ / ~RST~ */
    static char msg[48];
    snprintf(msg, sizeof(msg), "The game stopped (exit code %d).", r);
    fatal("GAME ERROR", msg);
}
