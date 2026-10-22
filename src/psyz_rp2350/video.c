/*
 * frank-castle — psyz video backend: PS1 display state, HDMI scanout, VSync.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The game double-buffers inside VRAM and tells the GPU which rectangle to
 * show (GP1 display area + display mode). Scanout reads that rectangle
 * straight from the VRAM model in PSRAM: for every source row a DMA copies the
 * pixels into an SRAM line buffer one row ahead of the beam, and the HSTX
 * line callback only hands that buffer over. 256/320/368-wide modes are
 * pixel-doubled to 640 by the HSTX DMA; 512/640-wide modes are shown 1:1.
 * Display parameters are latched at vblank, so a frame never tears.
 */

#include <psyz.h>
#include <psyz/video.h>
#include "draw.h"
#include "psx_video.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

extern u16 vram[VRAM_W * VRAM_H];

/* ---- display state written by the game (core 0) ---- */
static volatile int disp_x, disp_y;
static volatile int disp_w = 256, disp_h = 240;
static volatile bool disp_on = true;

/* ---- latched at vblank, read by the scanout (core 1 IRQ) ---- */
static volatile int frame_x, frame_y, frame_w = 256;
static volatile bool frame_on;
static volatile bool frame_native;   /* 512/640 modes: one VRAM pixel per output pixel */
static volatile int frame_copy_px;   /* pixels copied from VRAM per row */
static volatile int frame_src_x;     /* VRAM x of the first copied pixel */
static volatile int frame_pad;       /* black pixels before the copied ones */

/* Ring of line buffers filled by the core 1 main loop (PsxVideo_Core1Loop)
 * a few source rows ahead of the beam. The copy is done by the CPU through the
 * XIP cache, never by DMA: all DMA channels share one bus master, and a PSRAM
 * read stalled behind the game's own traffic would also stall the HSTX
 * channel and break HDMI timing. The HSTX IRQ only touches SRAM. */
#define NBUF 8
static uint16_t line_buf[NBUF][PSXV_OUT_W] __attribute__((aligned(4)));
static uint16_t black_line[PSXV_OUT_W] __attribute__((aligned(4)));
static volatile uint32_t vblank_count;
static volatile int beam_row;   /* source row being shown, -1 in vblank */
static volatile int fill_row;   /* next source row the loop will copy */
static volatile uint32_t frame_seq;
static volatile bool latch_request; /* display changed by the game */
volatile uint32_t psxv_late_rows; /* rows not copied in time (diagnostic) */

/* Like the PS1, a display change made during vblank (games flip right after
 * VSync returns) applies to the frame about to be shown; one made later
 * waits for the next vblank. The core 1 loop does the re-latch. */
void Draw_DisplayEnable(unsigned int on) {
    disp_on = on != 0;
    latch_request = true;
}

void Draw_DisplayArea(unsigned int x, unsigned int y) {
    disp_x = (int)(x & 1023);
    disp_y = (int)(y & 511);
    latch_request = true;
}

void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}

void Draw_DisplayVerticalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}

void Draw_SetDisplayMode(DisplayMode *mode) {
    static const int widths[4] = {256, 320, 512, 640};
    disp_w = mode->horizontal_resolution_368 ? 368 : widths[mode->horizontal_resolution & 3];
    disp_h = mode->vertical_resolution ? 480 : 240;
    latch_request = true;
}

void Draw_PutDispEnv(DISPENV *disp) { (void)disp; }

/* ------------------------------------------------------------------ */
/* Scanout (core 1)                                                    */
/* ------------------------------------------------------------------ */

/* A game may know better than the GP1 display area which buffer is complete
 * (SOTN flips with PutDispEnv only after VSync returns, often after active
 * video has begun). Return true and the origin to override. */
bool __attribute__((weak)) Psyz_DisplayOrigin(int *x, int *y) {
    (void)x;
    (void)y;
    return false;
}

static void __not_in_flash_func(latch_frame)(void) {
    int w = disp_w;
    int ox = disp_x, oy = disp_y;
    Psyz_DisplayOrigin(&ox, &oy);
    frame_on = disp_on;
    frame_x = ox & (VRAM_W - 1);
    frame_y = oy & (VRAM_H - 1);
    frame_w = w;
    frame_native = w >= 512;
    int out = frame_native ? PSXV_OUT_W : PSXV_OUT_W / 2;
    frame_copy_px = w > out ? out : w;
    frame_src_x = frame_x + (w > out ? (w - out) / 2 : 0); /* 368: show the middle 320 */
    if (frame_src_x + frame_copy_px > VRAM_W) frame_copy_px = VRAM_W - frame_src_x;
    frame_pad = (out - frame_copy_px) / 2;
    /* borders stay black: clear them whenever the geometry changes */
    static int last_pad = -1, last_px = -1;
    if (frame_pad != last_pad || frame_copy_px != last_px) {
        uint32_t *p = (uint32_t *)line_buf;
        for (unsigned i = 0; i < sizeof(line_buf) / 4; i++) p[i] = 0;
        last_pad = frame_pad;
        last_px = frame_copy_px;
    }
}

static void __not_in_flash_func(on_vsync)(void) {
    vblank_count++;
    latch_request = false;
    latch_frame();
    beam_row = -1;
    fill_row = 0;
    frame_seq++;
}

static void __not_in_flash_func(on_line)(uint32_t out_line, psxv_line_t *line) {
    if (!frame_on) {
        line->pixels = black_line;
        line->mode = PSXV_LINE_NATIVE;
        return;
    }
    int row = (int)(out_line >> 1);
    beam_row = row;
    if (row >= fill_row) psxv_late_rows++; /* shows whatever the slot held */
    line->pixels = line_buf[row & (NBUF - 1)];
    line->mode = frame_native ? PSXV_LINE_NATIVE : PSXV_LINE_DOUBLE;
}

void PsxVideo_Init(void) {
    psxv_set_line_callback(on_line);
    psxv_set_vsync_callback(on_vsync);
}

/* Core 1 thread: keep the ring filled ahead of the beam. The HSTX DMA reads a
 * slot up to two output lines after on_line handed it over, so the loop stays
 * NBUF-3 rows ahead at most: never refill a slot that may still be in flight. */
#define FILL_AHEAD (NBUF - 3)
void __not_in_flash_func(PsxVideo_Core1Loop)(void) {
    while (1) {
        if (latch_request && beam_row < 0) {
            /* Still in vblank: show the new display area this very frame.
             * No line IRQ runs during vblank, so this needs no locking, and
             * core 1 IRQs must never be masked: the HSTX ping-pong DMA
             * cannot tolerate a missed interrupt. */
            latch_request = false;
            latch_frame();
            fill_row = 0;
            frame_seq++;
        }
        uint32_t seq = frame_seq;
        int row = fill_row;
        if (!frame_on || row >= PSXV_OUT_H / 2 || row > beam_row + FILL_AHEAD) {
            tight_loop_contents();
            continue;
        }
        int y = (frame_y + row) & (VRAM_H - 1);
        const uint32_t *src = (const uint32_t *)&vram[y * VRAM_W + frame_src_x];
        uint16_t *dst16 = &line_buf[row & (NBUF - 1)][frame_pad];
        int n = frame_copy_px;
        if (!(((uintptr_t)src | (uintptr_t)dst16) & 3)) {
            uint32_t *dst = (uint32_t *)dst16;
            for (int i = 0; i < n / 2; i++) dst[i] = src[i];
        } else {
            const uint16_t *s16 = (const uint16_t *)src;
            for (int i = 0; i < n; i++) dst16[i] = s16[i];
        }
        /* a vblank in the middle of the copy restarts the frame */
        if (seq == frame_seq) fill_row = row + 1;
    }
}

/* ------------------------------------------------------------------ */
/* VSync / timing (core 0)                                             */
/* ------------------------------------------------------------------ */

static PsyzVsyncMode vsync_mode = PSYZ_VSYNC_AUTO;
static uint64_t last_vsync_us;

/* One video frame of SPU output (44100/60). Until the I2S mixer runs on core 1
 * the SPU is stepped here, which also advances the root counters the game's
 * sound driver ticks on. */
#define SPU_FRAMES_PER_VSYNC 735
static short spu_frame[SPU_FRAMES_PER_VSYNC * 2];
extern void Psyz_AudioLock(void);
extern void Psyz_AudioUnlock(void);
extern void Psyz_SpuPullSamples(short *out, int num_frames);

/* Frame statistics, logged every 2 s: game time per frame excluding the
 * vsync wait, worst frame, and frames actually presented per second. */
static uint64_t stat_t0, stat_work_us;
static uint32_t stat_frames, stat_worst_us, stat_vblank0;

static void frame_stats(uint64_t work_us) {
    stat_work_us += work_us;
    if (work_us > stat_worst_us) stat_worst_us = (uint32_t)work_us;
    stat_frames++;
    uint64_t now = time_us_64();
    if (!stat_t0) {
        stat_t0 = now;
        stat_vblank0 = vblank_count;
    } else if (now - stat_t0 >= 2000000) {
        extern volatile uint32_t psxv_late_rows;
        printf("[perf] %lu fps, game %lu us avg / %lu us worst, late rows %lu\n",
               (unsigned long)(stat_frames * 1000000ull / (now - stat_t0)),
               (unsigned long)(stat_work_us / stat_frames), (unsigned long)stat_worst_us,
               (unsigned long)psxv_late_rows);
        stat_t0 = now;
        stat_frames = 0;
        stat_work_us = 0;
        stat_worst_us = 0;
    }
}

int Psyz_VideoVSync(int mode) {
    if (mode < 0) return (int)vblank_count;
    /* horizontal lines since the last vsync, 63.5 us each (NTSC) */
    int lines = (int)(((time_us_64() - last_vsync_us) * 2) / 127) & 0xFFFF;
    if (mode == 0) {
        frame_stats(time_us_64() - last_vsync_us);
        Psyz_AudioLock();
        Psyz_SpuPullSamples(spu_frame, SPU_FRAMES_PER_VSYNC);
        Psyz_AudioUnlock();
        if (vsync_mode != PSYZ_VSYNC_LIMITLESS && vsync_mode != PSYZ_VSYNC_OFF) {
            uint32_t now = vblank_count;
            while (vblank_count == now) tight_loop_contents();
        }
        last_vsync_us = time_us_64();
    }
    return lines;
}

int Psyz_VideoSetVsyncMode(PsyzVsyncMode mode) {
    vsync_mode = mode;
    return 0;
}
PsyzVsyncMode Psyz_VideoGetVsyncMode(void) { return vsync_mode; }

int Psyz_VideoSetDitheringMode(PsyzDitherMode mode) {
    (void)mode;
    return 0;
}
PsyzDitherMode Psyz_VideoGetDitheringMode(void) { return PSYZ_DITHER_AUTO; }

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    return multiplier == 1 ? 0 : -1;
}
unsigned Psyz_VideoGetInternalResolution(void) { return 1; }

void Psyz_SetTitle(const char *str) { (void)str; }
