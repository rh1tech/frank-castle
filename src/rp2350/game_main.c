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

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

extern int sotn_main(int argc, char *argv[]);
extern bool InitPlatform(void);
extern unsigned short vram[1024 * 512];

extern void PsxVideo_Core1Loop(void);

static void core1_main(void) {
    psxv_start();
    PsxVideo_Core1Loop();
}

/* Before the game owns VRAM, messages are drawn straight into it. */
static void __attribute__((noreturn)) fatal(const char *what, const char *detail) {
    printf("FATAL: %s: %s\n", what, detail);
    fbcon_t con;
    fbcon_init(&con, vram, 320, 240, 1024);
    fbcon_clear(&con);
    con.fg = BGR555(255, 80, 80);
    fbcon_puts(&con, "\n  frank-castle\n\n  ");
    con.fg = BGR555(255, 255, 255);
    fbcon_puts(&con, what);
    fbcon_puts(&con, "\n\n  ");
    fbcon_puts(&con, detail);
    while (1) tight_loop_contents();
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

/* Split /castle/args.txt into argv entries (whitespace separated). */
static int read_extra_args(char *buf, size_t len, char **argv, int max) {
    FIL f;
    UINT got = 0;
    if (f_open(&f, "/castle/args.txt", FA_READ) != FR_OK) return 0;
    f_read(&f, buf, (UINT)len - 1, &got);
    f_close(&f);
    buf[got] = 0;
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

    if (platform_mount_sd() != FR_OK) fatal("SD card", "no readable FAT/exFAT filesystem");
    static char cue[160];
    if (!find_cue(cue, sizeof(cue)))
        fatal("Game disc not found", "copy the SLUS-00067 .cue/.bin to /castle/");
    if (!DiscProvider_Mount(&cd_fatfs_backend, cue)) fatal("Cannot read the disc image", cue);

    static char args_buf[256];
    static char *argv[16] = {"sotn", "--disk", cue};
    int argc = 3;
    argc += read_extra_args(args_buf, sizeof(args_buf), argv + argc, 16 - argc - 1);
    argv[argc] = NULL;

    printf("starting game:");
    for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");

    int r = sotn_main(argc, argv);
    printf("game exited (%d)\n", r);
    while (1) tight_loop_contents();
}
