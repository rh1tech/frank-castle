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
#include "vram_pitch.h"
#include "psx_video.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/structs/xip_aux.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/xip_cache.h"

#include <stdio.h>
#include <stdio.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

extern u16 g_PsxVram[VRAM_PITCH * VRAM_H];
#define vram g_PsxVram

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
/* 368/384 modes fill the 640 output columns like a TV does: the row lands at
 * the tail of its ring slot and is widened in place, nearest neighbour.
 * Safe in place: output pixel j reads tail pixel xmap[j] >= j. */
static volatile bool frame_expand;
static uint16_t xmap[PSXV_OUT_W];
/* when the frame is held in an SRAM framebuffer slot (gpu_fast.c) */
static const u16 *volatile frame_sram;
volatile uint32_t scan_sram, scan_psram; /* latches by source (diagnostic) */
static volatile int frame_sram_stride, frame_sram_r0, frame_sram_r1;

/* Ring of line buffers filled by the core 1 main loop (PsxVideo_Core1Loop)
 * a few source rows ahead of the beam. The copy is done by the CPU through the
 * XIP cache, never by DMA: all DMA channels share one bus master, and a PSRAM
 * read stalled behind the game's own traffic would also stall the HSTX
 * channel and break HDMI timing. The HSTX IRQ only touches SRAM. */
#define NBUF 14
static uint16_t line_buf[NBUF][PSXV_OUT_W] __attribute__((aligned(4)));
static uint16_t black_line[PSXV_OUT_W] __attribute__((aligned(4)));
static volatile uint32_t vblank_count;
volatile uint32_t psxv_late_by[8];
volatile int psxv_beam_row;    /* source row being shown, -1 in vblank */
volatile int psxv_fill_row;    /* next source row the loop will copy */
#define beam_row psxv_beam_row
#define fill_row psxv_fill_row
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

/* Display origin supplied by the game (Psyz_DisplayOrigin), sampled on the
 * game core at VSync: the scanout IRQ must not run flash code or touch
 * PSRAM, a stall there desyncs the HDMI DMA. */
static volatile int game_disp_x, game_disp_y;
static volatile bool game_disp_valid;

static void __not_in_flash_func(latch_frame)(void) {
    int w = disp_w;
    int ox = disp_x, oy = disp_y;
    if (game_disp_valid) {
        ox = game_disp_x;
        oy = game_disp_y;
    }
    frame_on = disp_on;
    frame_x = ox & (VRAM_W - 1);
    frame_y = oy & (VRAM_H - 1);
    frame_w = w;
    frame_expand = w == 368 || w == 384;
    frame_native = w >= 512 || frame_expand;
    int out = frame_native ? PSXV_OUT_W : PSXV_OUT_W / 2;
    frame_copy_px = w > out ? out : w;
    frame_src_x = frame_x + (w > out ? (w - out) / 2 : 0);
    if (frame_src_x + frame_copy_px > VRAM_W) frame_copy_px = VRAM_W - frame_src_x;
    frame_pad = frame_expand ? (out - frame_copy_px) & ~1 : (out - frame_copy_px) / 2;
    static int xmap_w;
    if (frame_expand && xmap_w != frame_copy_px) {
        xmap_w = frame_copy_px;
        for (int j = 0; j < PSXV_OUT_W; j++) xmap[j] = (uint16_t)(frame_pad + j * xmap_w / PSXV_OUT_W);
    }
    /* ring rows keep black borders: clear them whenever the geometry changes */
    static int last_pad = -1, last_px = -1;
    if (frame_pad != last_pad || frame_copy_px != last_px) {
        uint32_t *p = (uint32_t *)line_buf;
        for (unsigned i = 0; i < sizeof(line_buf) / 4; i++) p[i] = 0;
        last_pad = frame_pad;
        last_px = frame_copy_px;
    }
    extern bool Gpu_ScanoutSource(int x, int y, const u16 **row0, int *stride, int *r0, int *r1);
    const u16 *base;
    int stride, r0, r1;
    extern volatile uint32_t scan_sram, scan_psram;
    if (Gpu_ScanoutSource(frame_x, frame_y, &base, &stride, &r0, &r1)) {
        scan_sram++;
        frame_sram = frame_w == 256 ? base : NULL; /* complete 320-px lines */
        frame_sram_stride = stride;
        frame_sram_r0 = r0;
        frame_sram_r1 = r1;
    } else {
        scan_psram++;
        frame_sram = NULL;
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
    line->mode = frame_native ? PSXV_LINE_NATIVE : PSXV_LINE_DOUBLE;
    const u16 *sram = frame_sram;
    if (sram && row >= frame_sram_r0 && row < frame_sram_r1) {
        /* straight from the SRAM framebuffer: no copy, no deadline */
        line->pixels = sram + row * frame_sram_stride;
        return;
    }
    if (row >= fill_row) { /* shows whatever the slot held */
        extern volatile uint8_t gpu_c1_state;
        psxv_late_rows++;
        psxv_late_by[gpu_c1_state & 7]++;
    }
    line->pixels = line_buf[row % NBUF];
}

void PsxVideo_Init(void) {
    psxv_set_line_callback(on_line);
    psxv_set_vsync_callback(on_vsync);
}

/* The HSTX DMA reads a ring slot up to two output lines after on_line handed
 * it over, so filling stays NBUF-3 rows ahead at most: never refill a slot
 * that may still be in flight. */
#define FILL_AHEAD (NBUF - 3)

/* Rows scanned out of PSRAM are fetched by the XIP stream engine: it reads
 * in long QSPI bursts into a FIFO that a DREQ-paced DMA channel drains into
 * the ring. Reading through the cache costs one QSPI transaction per 8-byte
 * line and kept core 1 busy ~40% of the time in the 512-wide title mode;
 * the stream leaves core 1 free to rasterize. A DMA reading PSRAM directly
 * would stall the DMA master (and the HSTX channels) on every access, the
 * FIFO never does. One row is in flight at a time. */
static int stream_ch = -1;
static volatile int stream_row = -1; /* row in flight, -1: idle */
static uint32_t stream_row_seq;
volatile uint32_t psxv_stream_rows;

/* The channel is configured once (stream_setup); a transfer only sets the
 * destination and length: the rasterizer starts ~1500 row fetches a frame
 * on the title screen. */
static void stream_setup(void) {
    dma_channel_config c = dma_channel_get_default_config(stream_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_XIP_STREAM);
    dma_channel_configure(stream_ch, &c, NULL, &xip_aux_hw->stream, 0, false);
}

static inline void stream_start(const uint32_t *src, uint32_t *dst, uint32_t words) {
    dma_channel_set_write_addr(stream_ch, dst, false);
    dma_channel_set_trans_count(stream_ch, words, false);
    xip_ctrl_hw->stream_addr = (uint32_t)(uintptr_t)src;
    xip_ctrl_hw->stream_ctr = words;
    dma_channel_start(stream_ch);
}

volatile uint32_t psxv_stream_rescues;

static void stream_abort(void) {
    dma_channel_abort(stream_ch);
    xip_ctrl_hw->stream_ctr = 0;
    /* leftover words would be read first by the next stream */
    while (!(xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY_BITS)) (void)xip_aux_hw->stream;
}

/* Core 1, before the scanout loop starts: claim a channel and check that
 * the stream engine really reads the PSRAM window on this board. */
void Video_StreamInit(void) {
    static uint32_t probe[64] __attribute__((aligned(4)));
    uint32_t *ps = (uint32_t *)&vram[511 * VRAM_PITCH]; /* last VRAM row, unused at boot */
    uint32_t *ps_unc = (uint32_t *)((uintptr_t)ps + (0x15000000u - 0x11000000u));
    xip_cache_clean_range((uintptr_t)ps - XIP_BASE, sizeof(probe));
    xip_cache_invalidate_range((uintptr_t)ps - XIP_BASE, sizeof(probe));
    for (int i = 0; i < 64; i++) ps_unc[i] = 0xA5000000u ^ (uint32_t)i * 0x01010101u;
    stream_ch = dma_claim_unused_channel(false);
    if (stream_ch < 0) return;
    stream_setup();
    stream_start(ps, probe, 64);
    absolute_time_t t = make_timeout_time_us(1000);
    while (dma_channel_is_busy(stream_ch) && !time_reached(t)) tight_loop_contents();
    bool ok = !dma_channel_is_busy(stream_ch);
    for (int i = 0; ok && i < 64; i++) ok = probe[i] == (0xA5000000u ^ (uint32_t)i * 0x01010101u);
    for (int i = 0; i < 64; i++) ps_unc[i] = 0;
    printf("scanout: XIP stream %s\n", ok ? "ok" : "FAILED, using CPU copies");
    if (!ok) {
        dma_channel_abort(stream_ch);
        xip_ctrl_hw->stream_ctr = 0;
        dma_channel_unclaim(stream_ch);
        stream_ch = -1;
    }
}

static inline void __attribute__((always_inline)) expand_row(uint16_t *line) {
    for (int j = 0; j < PSXV_OUT_W; j += 4) {
        uint16_t a = line[xmap[j]], b = line[xmap[j + 1]], c = line[xmap[j + 2]], d = line[xmap[j + 3]];
        line[j] = a; line[j + 1] = b; line[j + 2] = c; line[j + 3] = d;
    }
}

/* The scanout row in flight has landed: account for it. */
static inline void stream_retire(void) {
    /* a vblank during the transfer restarted the frame: drop the row */
    if (stream_row_seq == frame_seq && fill_row == stream_row) {
        if (frame_expand) expand_row(line_buf[stream_row % NBUF]);
        fill_row = stream_row + 1;
    }
    stream_row = -1;
}

/* The rasterizer (core 1, like the scanout) borrows the stream engine to
 * fetch 8/15bpp texture rows into SRAM in one burst instead of a cache miss
 * per 8 bytes. One transfer at a time: a texture fetch waits for a scanout
 * row in flight, the scanout waits for a texture row (a few us). */
static bool tex_inflight;

volatile uint32_t tex_wait_us, tex_fetches; /* diagnostic */
bool __not_in_flash_func(Video_TexFetch)(const void *src, void *dst, uint32_t words) {
    if (stream_ch < 0) return false;
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy(stream_ch)) tight_loop_contents();
    tex_wait_us += time_us_32() - t0;
    tex_fetches++;
    if (stream_row >= 0) stream_retire();
    tex_inflight = true;
    stream_start((const uint32_t *)src, (uint32_t *)dst, words);
    return true;
}

void __not_in_flash_func(Video_TexWait)(void) {
    if (!tex_inflight) return;
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy(stream_ch)) tight_loop_contents();
    tex_wait_us += time_us_32() - t0;
    tex_inflight = false;
}

/* Copy the next scanout row into the ring if the beam needs it. Returns
 * false when the ring is far enough ahead (or nothing is shown, or the
 * row in flight has not landed yet). */
static inline bool __attribute__((always_inline)) fill_one(void) {
    if (stream_row >= 0) {
        /* the beam is about to reach the row in flight: wait for it rather
         * than issue more PSRAM reads that would starve the stream further */
        while (dma_channel_is_busy(stream_ch)) {
            if (stream_row_seq != frame_seq) {
                /* a row of the previous frame: drop it, row 0 is due */
                stream_abort();
                stream_row = -1;
                break;
            }
            if (stream_row > beam_row + 3) return false;
            if (stream_row <= beam_row + 2) {
                /* About to be shown. Stop issuing PSRAM reads: with core 1
                 * idle the stream gets the QMI and lands in a few tens of us,
                 * cheaper than copying the row by CPU. Rescue only a stream
                 * that is still stuck after 40 us. */
                uint32_t t0 = time_us_32();
                while (dma_channel_is_busy(stream_ch) && time_us_32() - t0 < 40) tight_loop_contents();
                if (!dma_channel_is_busy(stream_ch)) break;
                stream_abort();
                psxv_stream_rescues++;
                stream_row = -1;
                break;
            }
        }
        if (stream_row < 0) goto copy;
        stream_retire();
    }
copy:;
    uint32_t seq = frame_seq;
    int row = fill_row;
    if (!frame_on || row >= PSXV_OUT_H / 2 || row > beam_row + FILL_AHEAD) return false;
    bool urgent = row <= beam_row + 2;
    if (frame_sram && row >= frame_sram_r0 && row < frame_sram_r1) {
        /* the line callback reads this row straight from SRAM */
        if (seq == frame_seq) fill_row = row + 1;
        return true;
    }
    int y = (frame_y + row) & (VRAM_H - 1);
    const uint32_t *src = (const uint32_t *)&vram[y * VRAM_PITCH + frame_src_x];
    uint16_t *dst16 = &line_buf[row % NBUF][frame_pad];
    int n = frame_copy_px;
    if (stream_ch >= 0 && !urgent && !(((uintptr_t)src | (uintptr_t)dst16) & 3)) {
        /* the stream bypasses the cache: commit any pending writes first */
        xip_cache_clean_range((uintptr_t)src - XIP_BASE, (uint32_t)n * 2);
        if (tex_inflight) { /* a texture row is still coming in */
            while (dma_channel_is_busy(stream_ch)) tight_loop_contents();
            tex_inflight = false;
        }
        stream_row = row;
        stream_row_seq = seq;
        psxv_stream_rows++;
        stream_start(src, (uint32_t *)dst16, (uint32_t)n / 2);
        return true;
    }
    if (!(((uintptr_t)src | (uintptr_t)dst16) & 3)) {
        /* 8 words per step: back-to-back PSRAM reads keep the QMI busy */
        uint32_t *dst = (uint32_t *)dst16;
        int w = n / 2, i = 0;
        for (; i + 8 <= w; i += 8) {
            uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
            uint32_t e = src[i + 4], f = src[i + 5], g = src[i + 6], h = src[i + 7];
            dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d;
            dst[i + 4] = e; dst[i + 5] = f; dst[i + 6] = g; dst[i + 7] = h;
        }
        for (; i < w; i++) dst[i] = src[i];
    } else {
        const uint16_t *s16 = (const uint16_t *)src;
        for (int i = 0; i < n; i++) dst16[i] = s16[i];
    }
    if (frame_expand) expand_row(line_buf[row % NBUF]);
    /* a vblank in the middle of the copy restarts the frame */
    if (seq == frame_seq) fill_row = row + 1;
    return true;
}

/* Core 1, from inside long work (big primitives, slot copies): keep the
 * ring ahead of the beam so wide modes scanned from PSRAM never show stale
 * rows. Cheap when nothing is due (see Video_ScanPoll in video.h). */
void __not_in_flash_func(Video_ScanFill)(void) {
    while (fill_one()) {
    }
}

/* Core 1, between rows of a slot flush: the flush's back-to-back PSRAM
 * writes would starve the stream, so bring the ring fully up to date (and
 * wait for the row in flight) before every flush row. */
void __not_in_flash_func(Video_ScanSync)(void) {
    for (;;) {
        while (fill_one()) {
        }
        if (stream_row < 0) return;
        if (stream_row_seq != frame_seq || stream_row > beam_row + 6) return;
        while (dma_channel_is_busy(stream_ch)) tight_loop_contents();
    }
}

/* Core 1 thread: keep the ring filled ahead of the beam. The HSTX DMA reads a
 * slot up to two output lines after on_line handed it over, so the loop stays
 * NBUF-3 rows ahead at most: never refill a slot that may still be in flight. */
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
        if (!fill_one()) {
            /* scanout is ahead (or idle): rasterize */
            extern bool Gpu_Core1Work(uint32_t budget_us);
            if (!Gpu_Core1Work(150)) tight_loop_contents();
        }
    }
}

/* ------------------------------------------------------------------ */
/* VSync / timing (core 0)                                             */
/* ------------------------------------------------------------------ */

static PsyzVsyncMode vsync_mode = PSYZ_VSYNC_AUTO;
static uint64_t last_vsync_us;
volatile uint32_t psxv_vsync_ret_us; /* time_us_32 when VSync last returned */
static uint32_t vsync_entry_at_us, vsync_late, vsync_calls; /* timeline (perf log) */
static uint64_t vsync_entry_us;
static uint32_t wait_late, wait_late_vsync_us; /* reached the vblank wait after the vblank */
static uint32_t late_log[8][5]; /* late frames: VSync entry, DrawSync return, core 1 drain, mixing us, SD sectors */


/* Frame statistics, logged every 2 s: game time per frame excluding the
 * vsync wait, worst frame, and frames actually presented per second. */
static uint64_t stat_t0, stat_work_us, stat_spu_us;
extern volatile uint32_t psxv_resyncs;
extern volatile uint32_t gpu_prim_us, gpu_vram_us, gpu_prims, gpu_shadow_us, audio_underruns, audio_chunk_max_us;
static uint32_t stat_frames, stat_worst_us;

/* "~VERB~" on the UART: full diagnostics in the perf log (they take long
 * enough to print to cost a frame or two per report) */
volatile bool psxv_perf_verbose;
#define PV(...) do { if (psxv_perf_verbose) printf(__VA_ARGS__); } while (0)

static void frame_stats(uint64_t work_us) {
    stat_work_us += work_us;
    if (work_us > stat_worst_us) stat_worst_us = (uint32_t)work_us;
    stat_frames++;
#ifdef CASTLE_PROFILE
    {
        /* profile the first slow-raster frame after boot (stage banners etc.) */
        extern void profiler_trigger(void);
        static uint32_t prev_prim;
        uint32_t pu = gpu_prim_us;
        if (pu > prev_prim && pu - prev_prim > 8000) profiler_trigger();
        prev_prim = pu;
    }
#endif
    uint64_t now = time_us_64();
    if (!stat_t0) {
        stat_t0 = now;
    } else if (now - stat_t0 >= 2000000) {
        printf("[perf] %lu fps, game %lu us avg / %lu worst | raster %lu us (%lu prims) vram %lu us shadow %lu us spu %lu us | late rows %lu, audio underruns %lu, mix step max %lu us, scan sram/psram %lu/%lu, disp %d,%d mode %dx%d, hdmi resyncs %lu\n",
               (unsigned long)(stat_frames * 1000000ull / (now - stat_t0)),
               (unsigned long)(stat_work_us / stat_frames), (unsigned long)stat_worst_us,
               (unsigned long)(gpu_prim_us / stat_frames), (unsigned long)(gpu_prims / stat_frames),
               (unsigned long)(gpu_vram_us / stat_frames), (unsigned long)(gpu_shadow_us / stat_frames),
               (unsigned long)(stat_spu_us / stat_frames),
               (unsigned long)psxv_late_rows, (unsigned long)audio_underruns,
               (unsigned long)audio_chunk_max_us, (unsigned long)scan_sram, (unsigned long)scan_psram,
               frame_x, frame_y, frame_w, disp_h, (unsigned long)psxv_resyncs);
        scan_sram = scan_psram = 0;
        extern void Gpu_DebugArea(int *a);
        int a[6];
        Gpu_DebugArea(a);
        extern volatile uint32_t gpu_flush_cause[5];
        extern volatile uint32_t gpu_fence_us;
        extern volatile uint32_t psxv_irq_us, gpu_busy_us;
        PV("[perf] gpu fence wait %lu us/frame, core1: hdmi irq %lu%%, gpu work %lu%%\n",
               (unsigned long)(gpu_fence_us / stat_frames), (unsigned long)(psxv_irq_us / 20000),
               (unsigned long)(gpu_busy_us / 20000));
        extern volatile uint32_t gpu_sync_at_us, gpu_drain_at_us, gpu_tl_frames;
        if (gpu_tl_frames)
            PV("[perf] timeline from VSync: DrawSync at %lu us, core 1 drained at %lu us, VSync entered at %lu us (avg), %lu late, %lu VSync / %lu DrawSync\n",
                   (unsigned long)(gpu_sync_at_us / gpu_tl_frames), (unsigned long)(gpu_drain_at_us / gpu_tl_frames),
                   (unsigned long)(vsync_entry_at_us / (vsync_calls ? vsync_calls : 1)), (unsigned long)vsync_late,
                   (unsigned long)vsync_calls, (unsigned long)gpu_tl_frames);
        for (uint32_t k = 0; k < vsync_late && k < 8; k++)
            PV("[perf] late frame: VSync at %lu, DrawSync done %lu, core 1 drained %lu us, mixing %lu us, %lu SD sectors\n",
                   (unsigned long)late_log[k][0], (unsigned long)late_log[k][1], (unsigned long)late_log[k][2],
                   (unsigned long)late_log[k][3], (unsigned long)late_log[k][4]);
        if (wait_late)
            PV("[perf] %lu frames reached the vblank wait late, VSync work in them %lu us avg\n",
                   (unsigned long)wait_late, (unsigned long)(wait_late_vsync_us / wait_late));
        wait_late = wait_late_vsync_us = 0;
        vsync_entry_at_us = vsync_late = vsync_calls = 0;
        gpu_sync_at_us = gpu_drain_at_us = gpu_tl_frames = 0;
        extern volatile uint32_t gpu_class_us[12], gpu_class_n[12], gpu_class_px[12];
        {
            static const char *cls[12] = {"poly", "polyT", "polyTG", "line", "sprt", "tile",
                                          "poly~", "polyT~", "polyTG~", "line~", "sprt~", "tile~"};
            PV("[perf] raster us/frame by class:");
            for (int k = 0; k < 12; k++) {
                if (gpu_class_n[k])
                    PV(" %s %lu(%lu,%lupx)", cls[k], (unsigned long)(gpu_class_us[k] / stat_frames),
                           (unsigned long)(gpu_class_n[k] / stat_frames), (unsigned long)(gpu_class_px[k] / stat_frames));
                gpu_class_us[k] = gpu_class_n[k] = gpu_class_px[k] = 0;
            }
            PV("\n");
        }
        {
            extern unsigned soft_tri_setup_cyc, soft_tri_span_cyc, soft_tri_n;
            PV("[perf] triangles %u/frame: setup %u cycles each, spans %u cycles/frame\n",
                   soft_tri_n / stat_frames, soft_tri_n ? soft_tri_setup_cyc / soft_tri_n : 0,
                   soft_tri_span_cyc / stat_frames);
            soft_tri_n = soft_tri_setup_cyc = soft_tri_span_cyc = 0;

        }
        {
            extern volatile uint32_t flush_maint_us, flush_copy_us, flush_rows_skipped;
            PV("[perf] slot flush: cache maintenance %lu us, copy %lu us, %lu rows unchanged per frame\n",
               (unsigned long)(flush_maint_us / stat_frames), (unsigned long)(flush_copy_us / stat_frames),
               (unsigned long)(flush_rows_skipped / stat_frames));
            flush_maint_us = flush_copy_us = flush_rows_skipped = 0;
        }
        PV("[perf] texture row fetches %lu/frame, waiting %lu us/frame\n", (unsigned long)(tex_fetches / stat_frames),
           (unsigned long)(tex_wait_us / stat_frames));
        tex_fetches = tex_wait_us = 0;
        {
            extern int soft_semi_log[6][5], soft_semi_logged;
            for (int k = 0; k < soft_semi_logged; k++)
                PV("[perf] big semi sprite: bpp %d %dx%d colour %06x abr %d\n", soft_semi_log[k][0],
                   soft_semi_log[k][1], soft_semi_log[k][2], soft_semi_log[k][3], soft_semi_log[k][4]);
        }
        extern volatile uint32_t ds_site_pc[8], ds_site_n[8], ds_site_us[8];
        for (int i = 0; i < 8 && ds_site_pc[i]; i++) {
            PV("[perf] DrawSync from %08lx: %lu calls, %lu us waited\n", (unsigned long)ds_site_pc[i],
                   (unsigned long)ds_site_n[i], (unsigned long)ds_site_us[i]);
            ds_site_n[i] = ds_site_us[i] = 0;
        }
        extern volatile uint32_t audio_peak;
        extern unsigned psyz_xa_sectors, psyz_xa_stride_hits;
        PV("[perf] audio peak %lu, XA sectors %u (stride hits %u)\n", (unsigned long)audio_peak,
               psyz_xa_sectors, psyz_xa_stride_hits);
        audio_peak = 0;
        extern volatile uint32_t psxv_stream_rescues;
        PV("[perf] stream rescues %lu, late rows by core-1 state idle/prim/flush/load/clear/vram: %lu/%lu/%lu/%lu/%lu/%lu\n", (unsigned long)psxv_stream_rescues,
               (unsigned long)psxv_late_by[0], (unsigned long)psxv_late_by[1], (unsigned long)psxv_late_by[2],
               (unsigned long)psxv_late_by[3], (unsigned long)psxv_late_by[4], (unsigned long)psxv_late_by[5]);
        extern volatile uint32_t gpu_fence_by[6];
        PV("[perf] fence by sync/display/load/store/reset/other %lu/%lu/%lu/%lu/%lu/%lu us/frame\n",
               (unsigned long)(gpu_fence_by[0] / stat_frames), (unsigned long)(gpu_fence_by[1] / stat_frames),
               (unsigned long)(gpu_fence_by[2] / stat_frames), (unsigned long)(gpu_fence_by[3] / stat_frames),
               (unsigned long)(gpu_fence_by[4] / stat_frames), (unsigned long)(gpu_fence_by[5] / stat_frames));
        for (int k = 0; k < 6; k++) gpu_fence_by[k] = 0;
        gpu_fence_us = 0;
        psxv_irq_us = 0;
        gpu_busy_us = 0;
        PV("[perf] slot flush tex/read/write/evict %lu/%lu/%lu/%lu, loads %lu\n",
               (unsigned long)gpu_flush_cause[0], (unsigned long)gpu_flush_cause[1],
               (unsigned long)gpu_flush_cause[2], (unsigned long)gpu_flush_cause[3],
               (unsigned long)gpu_flush_cause[4]);
        for (int k = 0; k < 5; k++) gpu_flush_cause[k] = 0;
        extern volatile int gpu_last_alloc[4];
        extern volatile int gpu_allocs[8][5], gpu_alloc_n;
        for (int k = 0; k < gpu_alloc_n; k++)
            PV("[perf] slot alloc %d,%d %dx%d x%d\n", gpu_allocs[k][0], gpu_allocs[k][1],
                   gpu_allocs[k][2], gpu_allocs[k][3], gpu_allocs[k][4]);
        gpu_alloc_n = 0;
        audio_chunk_max_us = 0;
        gpu_prim_us = gpu_vram_us = gpu_prims = gpu_shadow_us = 0;
        stat_spu_us = 0;
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
        vsync_entry_us = time_us_64();
        vsync_entry_at_us += time_us_32() - psxv_vsync_ret_us;
        vsync_calls++;
        if ((time_us_64() - last_vsync_us) > 16600) {
            extern volatile uint32_t gpu_last_sync_at, gpu_last_drain_at;
            if (vsync_late < 8) {
                late_log[vsync_late][0] = (uint32_t)(time_us_64() - last_vsync_us);
                late_log[vsync_late][1] = gpu_last_sync_at;
                late_log[vsync_late][2] = gpu_last_drain_at;
                extern volatile uint32_t audio_frame_us, sd_frame_reads;
                late_log[vsync_late][3] = audio_frame_us;
                late_log[vsync_late][4] = sd_frame_reads;
            }
            vsync_late++;
        }
        extern void Gpu_FlushForDisplay(void);
        Gpu_FlushForDisplay();
        int gx, gy;
        if (Psyz_DisplayOrigin(&gx, &gy)) {
            game_disp_x = gx;
            game_disp_y = gy;
            game_disp_valid = true;
        }
        /* the SPU is mixed on core 1 (audio.c); its root counter ticks are
         * replayed here, on the game thread */
        extern void Audio_FrameFill(void);
        uint64_t t_spu = time_us_64();
        Audio_FrameFill();
        stat_spu_us += time_us_64() - t_spu;
        frame_stats(time_us_64() - last_vsync_us);
#ifdef CASTLE_PROFILE
        extern void profiler_poll(void);
        profiler_poll();
#endif
        {
            /* frames that reached the vblank wait too late: what VSync did */
            uint32_t at = (uint32_t)(time_us_64() - last_vsync_us);
            if (at > 16600) {
                wait_late++;
                wait_late_vsync_us += (uint32_t)(time_us_64() - vsync_entry_us);
            }
        }
        if (vsync_mode != PSYZ_VSYNC_LIMITLESS && vsync_mode != PSYZ_VSYNC_OFF) {
            /* wide modes: write the finished frame to PSRAM while waiting;
             * core 1 takes over whatever is left at the vblank */
            extern bool Gpu_FlushStep(void);
            uint32_t now = vblank_count;
            while (vblank_count == now)
                if (!Gpu_FlushStep()) tight_loop_contents();
        }
        extern void Gpu_FlushHandOff(void);
        Gpu_FlushHandOff();
        last_vsync_us = time_us_64();
        psxv_vsync_ret_us = (uint32_t)last_vsync_us;
        extern void platform_usb_poll(void);
        platform_usb_poll(); /* keyboard: once per frame, on the game thread */
        {
            extern volatile uint32_t audio_frame_us, sd_frame_reads;
            audio_frame_us = sd_frame_reads = 0;
        }
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
