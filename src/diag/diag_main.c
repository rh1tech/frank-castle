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
 * Phase 0 bring-up firmware. Exercises every peripheral the port needs:
 *   - HSTX 640x480 from BGR555 lines (pixel-doubled and native lines)
 *   - PSRAM pattern test and throughput at the target clocks
 *   - SD mount, CUE/ISO9660 index of /castle/*.cue, sector read speed
 *   - I2S 440 Hz tone at 44.1 kHz (T toggles)
 *   - USB keyboard events
 * Everything is shown on screen and mirrored to the UART.
 */

#include "board_config.h"
#include "platform.h"
#include "psram_init.h"
#include "psx_video.h"
#include "fbcon.h"
#include "cdimage.h"
#include "cd_fatfs.h"
#include "audio.h"
#include "usbhid.h"
#include "ff.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/clocks.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FB_W 320
#define FB_H 240

static uint16_t fb[FB_H][FB_W] __attribute__((aligned(4)));
static uint16_t native_line[PSXV_OUT_W] __attribute__((aligned(4)));
static fbcon_t con;

/* Bottom 16 output lines use a native 640-px line to prove 1:1 mode. */
#define NATIVE_FIRST_LINE (PSXV_OUT_H - 16)

static void __not_in_flash_func(line_cb)(uint32_t out_line, psxv_line_t *line) {
    if (out_line >= NATIVE_FIRST_LINE) {
        line->pixels = native_line;
        line->mode = PSXV_LINE_NATIVE;
    } else {
        line->pixels = fb[out_line >> 1];
        line->mode = PSXV_LINE_DOUBLE;
    }
}

static void core1_main(void) {
    psxv_start();
    while (1) tight_loop_contents();
}

/* ------------------------------------------------------------------ */

static void log_both(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_both(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fbcon_puts(&con, buf);
}

static void draw_test_pattern(void) {
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                       {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
    for (int i = 0; i < 8; i++)
        fbcon_fill_rect(&con, i * 40, 0, 40, 24, BGR555(bars[i][0], bars[i][1], bars[i][2]));
    /* 5-bit ramps: R, G, B, grey. Each step is 10 px wide. */
    for (int s = 0; s < 32; s++) {
        uint16_t v = (uint16_t)s;
        fbcon_fill_rect(&con, s * 10, 24, 10, 6, v);
        fbcon_fill_rect(&con, s * 10, 30, 10, 6, v << 5);
        fbcon_fill_rect(&con, s * 10, 36, 10, 6, v << 10);
        fbcon_fill_rect(&con, s * 10, 42, 10, 6, v | v << 5 | v << 10);
    }
    for (int x = 0; x < PSXV_OUT_W; x++)
        native_line[x] = (x & 1) ? 0x7fff : (x < 320 ? BGR555(255, 0, 0) : BGR555(0, 0, 255));
}

/* ------------------------------------------------------------------ */
/* PSRAM                                                               */
/* ------------------------------------------------------------------ */

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* Runs before the heap exists: clobbers all 8 MB. Writes and the first
 * verify go through the uncached alias (tests the chip, not the cache);
 * the second verify goes through the cache, as the game will. */

/* Flash timing sweep ('f' on the UART). Runs from SRAM with interrupts off;
 * reads go through the uncached XIP window, so code in the XIP cache keeps
 * running while a candidate timing is tried. */
static uint32_t __no_inline_not_in_flash_func(flash_sum_at)(uint32_t timing) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t saved = qmi_hw->m[0].timing;
    qmi_hw->m[0].timing = timing;
    const volatile uint32_t *p = (const volatile uint32_t *)0x14000000u; /* XIP no-cache alias */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 65536; i++) sum = sum * 31u + p[i];
    qmi_hw->m[0].timing = saved;
    restore_interrupts(irq);
    return sum;
}

static void flash_sweep(void) {
    uint32_t base = qmi_hw->m[0].timing;
    uint32_t ref = flash_sum_at(base);
    log_both("flash timing now %08lx\n", (unsigned long)base);
    static const int divs[] = {6, 5, 4, 3};
    for (unsigned k = 0; k < sizeof(divs) / sizeof(divs[0]); k++) {
        log_both("div %d (%d MHz):", divs[k], (int)(clock_get_hz(clk_sys) / 1000000 / divs[k]));
        for (uint32_t rx = 1; rx < 8; rx++) {
            uint32_t t = (base & ~(QMI_M0_TIMING_RXDELAY_BITS | QMI_M0_TIMING_CLKDIV_BITS)) |
                         rx << QMI_M0_TIMING_RXDELAY_LSB | (uint32_t)divs[k] << QMI_M0_TIMING_CLKDIV_LSB;
            log_both(" rx%lu:%s", (unsigned long)rx, flash_sum_at(t) == ref ? "ok" : "--");
        }
        log_both("\n");
    }
}

static void psram_test(void) {
    volatile uint32_t *cached = (volatile uint32_t *)0x11000000;
    volatile uint32_t *uncached = (volatile uint32_t *)0x15000000;
    const uint32_t words = 8u * 1024 * 1024 / 4;
    uint32_t errors = 0, first_bad = 0, x;

    uint64_t t0 = time_us_64();
    x = 0x12345678;
    for (uint32_t i = 0; i < words; i++) {
        x = xorshift(x);
        uncached[i] = x;
    }
    uint64_t t1 = time_us_64();
    x = 0x12345678;
    for (uint32_t i = 0; i < words; i++) {
        x = xorshift(x);
        if (uncached[i] != x && !errors++) first_bad = i * 4;
    }
    uint64_t t2 = time_us_64();
    x = 0x12345678;
    for (uint32_t i = 0; i < words; i++) {
        x = xorshift(x);
        if (cached[i] != x && !errors++) first_bad = i * 4;
    }
    uint64_t t3 = time_us_64();

    log_both("PSRAM 8MB @%d MHz: %s", PSRAM_MAX_FREQ_MHZ, errors ? "FAIL" : "OK");
    if (errors) log_both(" (%lu bad, first @0x%06lx)", (unsigned long)errors, (unsigned long)first_bad);
    log_both("\n  KB/s: wr %lu, rd %lu, cached rd %lu\n",
             (unsigned long)(8192ull * 1000000 / (t1 - t0 + 1)),
             (unsigned long)(8192ull * 1000000 / (t2 - t1 + 1)),
             (unsigned long)(8192ull * 1000000 / (t3 - t2 + 1)));
}

/* ------------------------------------------------------------------ */
/* SD + disc image                                                     */
/* ------------------------------------------------------------------ */

static bool find_cue(char *out, size_t outlen) {
    static const char *dirs[] = {"/castle", "/CASTLE"};
    for (int d = 0; d < 2; d++) {
        FFDIR dir;
        FILINFO fi;
        if (f_opendir(&dir, dirs[d]) != FR_OK) continue;
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            size_t n = strlen(fi.fname);
            if (n > 4 && (strcmp(fi.fname + n - 4, ".cue") == 0 || strcmp(fi.fname + n - 4, ".CUE") == 0)) {
                snprintf(out, outlen, "%s/%s", dirs[d], fi.fname);
                f_closedir(&dir);
                return true;
            }
        }
        f_closedir(&dir);
    }
    return false;
}

static void disc_test(void) {
    if (platform_mount_sd() != FR_OK) {
        log_both("SD: mount FAILED\n");
        return;
    }
    char cue[128];
    if (!find_cue(cue, sizeof(cue))) {
        log_both("SD: OK, but no /castle/*.cue\n");
        return;
    }
    uint64_t t0 = time_us_64();
    cd_result_t r = cd_open(&cd_fatfs_backend, cue);
    uint64_t t1 = time_us_64();
    if (r != CD_OK) {
        log_both("CD: %s open error %d\n", cue, r);
        return;
    }
    log_both("CD: %s\n  vol '%s', %lu files, %s, index %lu ms\n", cue, cd_volume_id(),
             (unsigned long)cd_entry_count(), cd_has_raw_sectors() ? "raw 2352" : "2048",
             (unsigned long)((t1 - t0) / 1000));

    static const char *probe[] = {"BIN/F_GAME.BIN", "DRA.BIN", "ST/WRP/F_WRP.BIN", "ST/WRP/WRP.BIN"};
    for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        const cd_entry_t *e = cd_lookup(probe[i]);
        log_both("  %-18s %s", probe[i], e ? "" : "missing\n");
        if (e) log_both("lba %6lu %7lu B\n", (unsigned long)e->lba, (unsigned long)e->size);
    }
    for (uint32_t i = 0; i < cd_entry_count(); i++) {
        const cd_entry_t *e = cd_entry_at(i);
        if (!strchr(e->path, '/'))
            printf("  /%s lba=%lu size=%lu\n", e->path, (unsigned long)e->lba, (unsigned long)e->size);
    }

    /* Sequential read throughput into SRAM: 256 data sectors = 512 KB. */
    static uint8_t buf[16 * CD_SECTOR_DATA];
    uint32_t lba = 16, total = 0;
    t0 = time_us_64();
    for (int i = 0; i < 16; i++) {
        uint32_t got = cd_read_data(lba, 16, buf);
        total += got;
        lba += got;
        if (got < 16) break;
    }
    t1 = time_us_64();
    log_both("  read %lu KB at %lu KB/s\n", (unsigned long)(total * 2),
             (unsigned long)((uint64_t)total * 2 * 1000000 / (t1 - t0 + 1)));
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

#define TONE_RATE 44100
static int16_t sine[256];
static volatile bool tone_on = true;
static uint32_t phase;

static void __not_in_flash_func(tone_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    int16_t *s = (int16_t *)buf;
    const uint32_t step = (uint32_t)(440.0 * 4294967296.0 / TONE_RATE);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = tone_on ? sine[phase >> 24] : 0;
        phase += step;
        s[2 * i] = v;
        s[2 * i + 1] = v;
    }
}

static void audio_test(void) {
    for (int i = 0; i < 256; i++) sine[i] = (int16_t)(8000 * sinf(i * 6.2831853f / 256));
    static i2s_config_t cfg;
    cfg.sample_freq = TONE_RATE;
    cfg.channel_count = 2;
    cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
    cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    cfg.pio = pio0;
    cfg.dma_trans_count = 441; /* 10 ms */
    cfg.volume = 0;
    i2s_init(&cfg);
    i2s_set_fill_callback(tone_fill);
    i2s_start();
    log_both("I2S: 440 Hz @ %d Hz (T toggles)\n", TONE_RATE);
}

/* ------------------------------------------------------------------ */

int main(void) {
    platform_init_clocks();
    platform_report_previous_crash();

    fbcon_init(&con, &fb[0][0], FB_W, FB_H, FB_W);
    fbcon_clear(&con);
    draw_test_pattern();
    fbcon_goto(&con, 0, 7);

    /* Video first so every later step is visible. */
    psxv_init();
    psxv_set_line_callback(line_cb);
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    log_both("frank-castle diag %s  sys %lu MHz\n", CASTLE_VERSION,
             (unsigned long)(clock_get_hz(clk_sys) / 1000000));

    psram_init(get_psram_pin());
    psram_test();
    platform_init_psram();

    disc_test();
    audio_test();
    platform_init_usb();
    log_both("USB host up. Press keys.\n");

    uint32_t last_frames = psxv_frame_count;
    uint64_t last_t = time_us_64();
    while (1) {
        /* UART: "~BOOT~" reboots to BOOTSEL, 'm' re-runs the PSRAM test */
        int ch = getchar_timeout_us(0);
        if (ch >= 0) {
            extern void platform_uart_cmd(int ch);
            platform_uart_cmd(ch);
            if (ch == 'm') psram_test();
            if (ch == 'f') flash_sweep();
            if (ch == 'r') {
                /* sweep the PSRAM read sampling delay at the current clock */
                for (uint32_t d = 0; d < 8; d++) {
                    uint32_t t = qmi_hw->m[1].timing;
                    qmi_hw->m[1].timing = (t & ~QMI_M1_TIMING_RXDELAY_BITS) | (d << QMI_M1_TIMING_RXDELAY_LSB);
                    log_both("rxdelay %lu: ", (unsigned long)d);
                    psram_test();
                }
            }
        }
        uint8_t key;
        int down;
        while (usbhid_get_key_action(&key, &down)) {
            log_both("key 0x%02x %s\n", key, down ? "down" : "up");
            if (down && key == 0x17) tone_on = !tone_on; /* HID 'T' */
        }
        uint64_t now = time_us_64();
        if (now - last_t >= 1000000) {
            uint32_t f = psxv_frame_count;
            char buf[48];
            snprintf(buf, sizeof(buf), " %2lu fps  kbd:%s ", (unsigned long)(f - last_frames),
                     usbhid_keyboard_connected() ? "yes" : "no ");
            int col = con.col, row = con.row;
            fbcon_goto(&con, (FB_W / 6) - (int)strlen(buf), 6);
            fbcon_puts(&con, buf);
            fbcon_goto(&con, col, row);
            last_frames = f;
            last_t = now;
        }
        tight_loop_contents();
    }
}
