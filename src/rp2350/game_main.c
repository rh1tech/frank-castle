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

#include "pico/multicore.h"
#include "pico/stdlib.h"

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

extern void Audio_Init(void);

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

/* Before the game owns VRAM, messages are drawn straight into it. */
static void __attribute__((noreturn)) fatal(const char *what, const char *detail) {
    printf("FATAL: %s: %s\n", what, detail);
    fbcon_t con;
    fbcon_init(&con, g_PsxVram, 320, 240, VRAM_PITCH);
    fbcon_clear(&con);
    con.fg = BGR555(255, 80, 80);
    fbcon_puts(&con, "\n  frank-castle\n\n  ");
    con.fg = BGR555(255, 255, 255);
    fbcon_puts(&con, what);
    fbcon_puts(&con, "\n\n  ");
    fbcon_puts(&con, detail);
    while (1) {
        int ch = getchar_timeout_us(1000);
        if (ch >= 0) platform_uart_cmd(ch); /* ~BOOT~ still works here */
    }
}

/* The game walked into an area whose stage is not built yet (sim_pc.c). */
void Platform_StageMissing(const char *name) {
    static char msg[48];
    snprintf(msg, sizeof(msg), "stage '%s' is not in this build", name);
    fatal("Area not available yet", msg);
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
    if (uart_args(buf, len)) {
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

int main(void) {
    platform_init_clocks();
    platform_report_previous_crash();
    platform_init_psram();

    /* Scanout reads the PS1 VRAM model; start it before anything can fail. */
    psxv_init();
    InitPlatform();
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    platform_init_usb();
    Audio_Init(); /* I2S IRQ on core 0; the SPU mixer runs on core 1 */

    if (platform_mount_sd() != FR_OK) fatal("SD card", "no readable FAT/exFAT filesystem");
    static char cue[160];
    if (!find_cue(cue, sizeof(cue)))
        fatal("Game disc not found", "copy the SLUS-00067 .cue/.bin to /castle/");
    /* SD cards occasionally drop out right after a warm reset (or when a USB
     * device enumerates): re-initialise the card and mount again. */
    for (int attempt = 0; !DiscProvider_Mount(&cd_fatfs_backend, cue); attempt++) {
        if (attempt == 3) fatal("Cannot read the disc image", cue);
        printf("disc mount failed, re-initialising the SD card (%d)\n", attempt + 1);
        sleep_ms(200 << attempt);
        disk_initialize(0);
        platform_mount_sd();
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
    static char msg[32];
    snprintf(msg, sizeof(msg), "exit code %d", r);
    fatal("Game exited", msg);
}
