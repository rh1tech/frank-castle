/*
 * frank-castle — PS1 GPU for RP2350: shell around soft_raster.inc.c.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later (this shell; the included
 * rasterizer core is MPL-2.0)
 *
 * VRAM is a 1024x512 BGR555 array in PSRAM. The rasterizer loops run from
 * SRAM and CLUTs are cached in SRAM.
 *
 * Framebuffer slots: PSRAM costs ~200 ns per scattered 16-bit access (8-byte
 * XIP cache lines, thrashed by code fetches), so framebuffers live in a
 * 256 KB SRAM area split into two slots. A game double-buffers between two
 * VRAM rectangles; each gets a slot. The rasterizer draws into the slot of
 * the current draw area and the HDMI scanout reads the displayed one
 * straight from SRAM (Gpu_ScanoutRow). A slot is written back to PSRAM only
 * when something needs the PSRAM copy: a texture or CLUT fetch, a VRAM block
 * op touching it, or the slot being reused for another rectangle. A buffer
 * too large for half the area (512x240 title screen) takes the whole area,
 * and is then written back at every VSync so the scanout reads PSRAM.
 */

#include <psyz.h>
#include <libgpu.h>
#include <stdbool.h>
#include <string.h>
#include "internal.h"
#include "draw.h"
#include "vram_pitch.h"

#include "hardware/regs/addressmap.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"

typedef long long s64;

u16 g_PsxVram[VRAM_PITCH * VRAM_H] __attribute__((section(".psram_bss.vram"), aligned(64)));

/* ---- framebuffer slots ---- */
/* A half slot stores each row as a complete 320-pixel HDMI line: the draw
 * area centred between black margins, so the scanout can DMA SRAM rows
 * as-is. Sized for SOTN's 256x207 letterboxed play area (320x208); together
 * the two halves hold a 512x240 buffer. */
#define LINE_PX 320
#define SLOT_PIXELS (LINE_PX * 208)
#define SHADOW_PIXELS (2 * SLOT_PIXELS)
static u16 shadow_buf[SHADOW_PIXELS] __attribute__((aligned(4)));
/* Small offscreen render targets (SOTN draws a 63x63 one every scrolling
 * frame) get their own slot instead of evicting a framebuffer. */
#define AUX_PIXELS (64 * 64) /* SOTN's offscreen target is 63x63 */
#define FB_MIN_PIXELS 32768 /* draw areas this big are framebuffers */
static u16 aux_buf[AUX_PIXELS] __attribute__((aligned(4)));
#define NSLOT 3

typedef struct {
    int x0, y0, w, h;
    int sw;     /* row stride in pixels, even */
    int pad;    /* x0 & 1: pixel (x,y) keeps the address parity it has in VRAM */
    int margin; /* black pixels left of the area (half slots of width <= 320) */
    u16 *base;  /* first pixel of row 0 */
    bool used;
    bool whole; /* occupies the whole area (too big for a half) */
    bool loaded; /* holds the rectangle's current content */
    bool dirty;  /* newer than PSRAM */
} Slot;

static Slot slot[NSLOT];
/* per core (core 0 rasterizes too while it helps, see gpu_help): */
static int draw_cur_c[2] = {-1, -1}; /* slot DROW() draws into, -1 = PSRAM */
static u16 *draw_origin_c[2];        /* draw_origin + y*sw + x == pixel (x,y) */
static int draw_sw_c[2];
volatile uint32_t gpu_shadow_us;
/* why slots were flushed/loaded (diagnostic): tex, read, write, evict, load */
volatile uint32_t gpu_flush_cause[5];
volatile int gpu_last_alloc[4];
volatile int gpu_allocs[8][5], gpu_alloc_n;
#define CAUSE_TEX 0
#define CAUSE_READ 1
#define CAUSE_WRITE 2
#define CAUSE_EVICT 3
#define CAUSE_LOAD 4

/* Long work on core 1 (a full-screen sprite, a 240 KB slot copy) would
 * starve the scanout ring in the wide modes scanned out of PSRAM: poll it
 * once per row. Only core 1 rasterizes; the check is two SRAM loads. */
extern volatile int psxv_beam_row, psxv_fill_row;
extern void Video_ScanFill(void);
extern void Video_ScanSync(void);
static inline void scan_poll(void) {
    if (psxv_fill_row <= psxv_beam_row + 10 && psxv_fill_row < 240 && get_core_num() == 1) Video_ScanFill();
}
static inline u16 *drow(int y) {
    int c = get_core_num();
    scan_poll();
    return draw_cur_c[c] >= 0 ? draw_origin_c[c] + y * draw_sw_c[c] : g_PsxVram + y * VRAM_PITCH;
}
#define DROW(y) drow(y)

/* core 0 helping (see gpu_help): wait until it has drawn everything core 1
 * published, before core 1 changes slots or VRAM under it */
static void help_sync(int site);

static bool overlaps(const Slot *s, int x, int y, int w, int h) {
    return s->used && x < s->x0 + s->w && x + w > s->x0 && y < s->y0 + s->h && y + h > s->y0;
}

/* Bulk copies between a slot and PSRAM go through the uncached alias
 * (0x15xxxxxx): through the cache every 8-byte line costs a write-allocate
 * read plus a write-back. Each PSRAM row is cleaned (pending writes
 * committed) and, before being written, invalidated so no stale line can be
 * read or evicted over the new data. */
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)
#define XIP_LINE 8u

static inline void row_cache_sync(const u16 *p, int px, bool invalidate) {
    uintptr_t off = (uintptr_t)p - XIP_BASE;
    uintptr_t a = off & ~(uintptr_t)(XIP_LINE - 1);
    uintptr_t b = (off + (uintptr_t)px * 2 + XIP_LINE - 1) & ~(uintptr_t)(XIP_LINE - 1);
    xip_cache_clean_range(a, b - a);
    if (invalidate) xip_cache_invalidate_range(a, b - a);
}

static inline void copy_px(u16 *dst, const u16 *src, int n) {
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3)) {
        uint32_t *d = (uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (int i = 0; i < n / 2; i++) d[i] = s[i];
        if (n & 1) dst[n - 1] = src[n - 1];
    } else {
        for (int i = 0; i < n; i++) dst[i] = src[i];
    }
}

/* what core 1 is doing, for the late-row breakdown in the perf log:
 * 0 idle, 1 primitive, 2 slot flush, 3 slot load, 4 clear, 5 other VRAM op */
volatile uint8_t gpu_c1_state;

volatile uint32_t flush_maint_us, flush_copy_us, flush_rows_skipped; /* diagnostic */

/* What each 64-pixel segment of each VRAM row in PSRAM last received from a
 * slot flush: the writing rectangle's signature (x0 << 16 | w, 0: unknown)
 * and a hash of the pixels. Title and menu screens redraw the same backdrop
 * every frame, and with double buffering a PSRAM buffer was written two
 * frames ago with mostly the same content: unchanged segments are skipped
 * (the title's animated overlays cover only part of each row). Anything
 * else writing PSRAM VRAM forgets the segments it touches (vrow_forget).
 * The tables live in PSRAM: a few accesses per row, and SRAM is full. */
#define SEG_PX 64
#define SEGS (VRAM_W / SEG_PX)
static uint32_t seg_hash[VRAM_H][SEGS] __attribute__((section(".psram_bss.seghash")));
static uint32_t seg_sig[VRAM_H][SEGS] __attribute__((section(".psram_bss.seghash")));

static void soft_tex_forget(int x, int y, int w, int h);
static void vrow_forget(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    soft_tex_forget(x, y, w, h); /* SRAM texture copies of it are stale */
    if (h > VRAM_H) h = VRAM_H;
    int k0 = x < 0 ? 0 : x / SEG_PX, k1 = (x + w - 1) / SEG_PX;
    if (k1 >= SEGS) k1 = SEGS - 1;
    for (int j = 0; j < h; j++) {
        int r = (y + j) & (VRAM_H - 1);
        for (int k = k0; k <= k1; k++) seg_sig[r][k] = 0;
    }
}

static inline uint32_t seg_hash16(const u16 *p, int n) {
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
    return h;
}

/* Slot rows go to PSRAM by CPU stores. Not by DMA: a DMA channel writing
 * PSRAM stalls the DMA master on every write and the HSTX channels with it
 * (tried: 2.5x faster copy, no HDMI signal). */
static void flush_rows(Slot *s, int jfrom, int jto) {
    uint32_t sig = (uint32_t)s->x0 << 16 | (uint32_t)s->w;
    if (jto > jfrom) soft_tex_forget(s->x0, s->y0 + jfrom, s->w, jto - jfrom);
    int xend = s->x0 + s->w;
    for (int j = jfrom; j < jto; j++) {
        int vy = (s->y0 + j) & (VRAM_H - 1);
        u16 *row = g_PsxVram + vy * VRAM_PITCH;
        const u16 *src = s->base + j * s->sw - s->x0; /* src[x] = pixel (x, row) */
        if (get_core_num() == 1) Video_ScanSync();
        for (int xa = s->x0; xa < xend;) {
            int k = xa / SEG_PX;
            int xb = (k + 1) * SEG_PX;
            if (xb > xend) xb = xend;
            uint32_t h = seg_hash16(src + xa, xb - xa);
            if (seg_sig[vy][k] == sig && seg_hash[vy][k] == h) {
                flush_rows_skipped++; /* PSRAM already holds exactly this */
            } else {
                seg_sig[vy][k] = sig;
                seg_hash[vy][k] = h;
                uint32_t c0 = time_us_32();
                row_cache_sync(row + xa, xb - xa, true);
                uint32_t c1 = time_us_32();
                copy_px((u16 *)((uintptr_t)(row + xa) + PSRAM_UNCACHED_OFFSET), src + xa, xb - xa);
                flush_maint_us += c1 - c0;
                flush_copy_us += time_us_32() - c1;
            }
            xa = xb;
        }
    }
}

/* Display flush split across the frame boundary: core 0 flushes rows while
 * it waits for vblank in VSync (Gpu_FlushStep), and whatever is left when
 * the vblank comes is finished by core 1 before it draws anything else
 * (Gpu_Core1FlushPending). Those are bottom rows; the beam reaches them
 * ~8 ms later. Only one core flushes at a time: core 0 until it hands over
 * (fl_core1), core 1 after. */
static volatile int fl_slot = -1; /* slot whose display flush is in progress */
static volatile int fl_next;      /* first row not flushed yet */
static volatile bool fl_core1;    /* core 0 handed the rest to core 1 */

volatile uint32_t frame_c1flush_us, frame_raster_us, frame_c1flush_rows; /* this frame (reset at VSync) */

static void slot_flush_c(Slot *s, int cause) {
    if (!s->used || !s->dirty || !s->loaded) return;
    help_sync(0);
    uint8_t prev_state = gpu_c1_state;
    gpu_c1_state = 2;
    gpu_flush_cause[cause]++;
    uint64_t t = time_us_64();
    int start = 0;
    if (fl_slot == (int)(s - slot)) { /* finish a display flush in progress */
        start = fl_next;
        fl_slot = -1;
        fl_core1 = false;
    }
    flush_rows(s, start, s->h);
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
    gpu_c1_state = prev_state;
}

#define slot_flush(s) slot_flush_c(s, CAUSE_EVICT)

static void slot_load(Slot *s) {
    help_sync(1);
    uint8_t prev_state = gpu_c1_state;
    gpu_c1_state = 3;
    gpu_flush_cause[CAUSE_LOAD]++;
    uint64_t t = time_us_64();
    for (int j = 0; j < s->h; j++) {
        const u16 *row = g_PsxVram + (s->y0 + j) * VRAM_PITCH + s->x0;
        scan_poll();
        row_cache_sync(row, s->w, false);
        copy_px(s->base + j * s->sw, (const u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), s->w);
    }
    s->loaded = true;
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
    gpu_c1_state = prev_state;
}

static void slot_release(Slot *s) {
    slot_flush(s);
    s->used = false;
}

static void flush_overlapping(int x, int y, int w, int h) {
    for (int i = 0; i < NSLOT; i++)
        if (overlaps(&slot[i], x, y, w, h)) slot_flush_c(&slot[i], CAUSE_READ);
}

/* the rectangle the scanout will show next (see video.c) */
extern bool Psyz_DisplayOrigin(int *x, int *y);
static bool is_display_rect(const Slot *s) {
    int dx = -1, dy = -1;
    return Psyz_DisplayOrigin(&dx, &dy) && dx == s->x0 && s->y0 >= dy && s->y0 < dy + 240;
}

/* Find or allocate the slot for rect; -1 if it does not fit. */
static int slot_for(int x, int y, int w, int h) {
    for (int i = 0; i < NSLOT; i++)
        if (slot[i].used && slot[i].x0 == x && slot[i].y0 == y && slot[i].w == w && slot[i].h == h)
            return i;
    /* an area inside a slot draws there: the Colosseum alternates 256x207
     * and 256x167 areas at one origin, and a new slot for each meant a
     * 53k-pixel eviction and reload every switch */
    for (int i = 0; i < NSLOT; i++)
        if (slot[i].used && x >= slot[i].x0 && y >= slot[i].y0 && x + w <= slot[i].x0 + slot[i].w &&
            y + h <= slot[i].y0 + slot[i].h)
            return i;
    int pad = x & 1, sw = (w + pad + 1) & ~1, margin = 0;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H ||
        sw * h > SHADOW_PIXELS)
        return -1;
    help_sync(2); /* a new slot replaces another */
    if (w * h < FB_MIN_PIXELS) {
        /* small offscreen target: the aux slot, or PSRAM if too big for it */
        if (sw * h > AUX_PIXELS) return -1;
        slot_release(&slot[2]);
        slot[2] = (Slot){x, y, w, h, sw, pad, 0, aux_buf + pad, true, false, false, false};
        return 2;
    }
    if (w + pad <= LINE_PX && LINE_PX * h <= SLOT_PIXELS) {
        margin = (LINE_PX - w) / 2 & ~1;
        sw = LINE_PX;
    }
    bool whole = sw * h > SLOT_PIXELS;
    if (whole) {
        margin = 0;
        sw = (w + pad + 1) & ~1;
    }
    int victim;
    if (whole) {
        slot_release(&slot[0]);
        slot_release(&slot[1]);
        victim = 0;
    } else {
        if (slot[0].whole) slot_release(&slot[0]);
        /* never take the slot about to be shown */
        victim = !slot[0].used ? 0 : !slot[1].used ? 1 : is_display_rect(&slot[0]) ? 1 : 0;
        slot_release(&slot[victim]);
    }
    gpu_last_alloc[0] = x; gpu_last_alloc[1] = y; gpu_last_alloc[2] = w; gpu_last_alloc[3] = h;
    {
        bool seen = false;
        for (int k = 0; k < gpu_alloc_n; k++)
            if (gpu_allocs[k][0] == x && gpu_allocs[k][1] == y && gpu_allocs[k][2] == w && gpu_allocs[k][3] == h) {
                gpu_allocs[k][4]++;
                seen = true;
            }
        if (!seen && gpu_alloc_n < 8) {
            int *e = (int *)gpu_allocs[gpu_alloc_n++];
            e[0] = x; e[1] = y; e[2] = w; e[3] = h; e[4] = 1;
        }
    }
    Slot *s = &slot[victim];
    u16 *mem = shadow_buf + (whole ? 0 : victim * SLOT_PIXELS);
    *s = (Slot){x, y, w, h, sw, pad, margin, mem + margin + pad, true, whole, false, false};
    if (margin) memset(mem, 0, (size_t)sw * h * 2); /* black margins */
    return victim;
}

/* point this core's DROW() at slot i (-1: PSRAM) */
static void draw_into(int i) {
    int c = get_core_num();
    draw_cur_c[c] = i;
    if (i < 0) return;
    Slot *s = &slot[i];
    draw_sw_c[c] = s->sw;
    draw_origin_c[c] = s->base - s->y0 * s->sw - s->x0;
}

volatile uint16_t tex_flush_log[6]; /* perf log: the last texture flush */
/* Before sampling texels/CLUT that live in a slot, publish it to PSRAM. */
static void tex_hook(u16 tpage, u16 clut) {
    int bpp = (tpage >> 7) & 3;
    int px = (tpage & 0xF) * 64, py = ((tpage >> 4) & 1) * 256;
    int pw = bpp == 0 ? 64 : bpp == 1 ? 128 : 256;
    for (int i = 0; i < NSLOT; i++)
        if (slot[i].dirty && (overlaps(&slot[i], px, py, pw, 256) ||
                              overlaps(&slot[i], (clut & 0x3F) * 16, (clut >> 6) & 0x1FF, 256, 1)))
        {
            tex_flush_log[0] = tpage, tex_flush_log[1] = clut, tex_flush_log[2] = (uint16_t)slot[i].x0;
            tex_flush_log[3] = (uint16_t)slot[i].y0, tex_flush_log[4] = (uint16_t)slot[i].w, tex_flush_log[5] = (uint16_t)slot[i].h;
            slot_flush_c(&slot[i], CAUSE_TEX);
        }
}
/* core 1 publishes slots before core 0 may sample them (gpu_help) */
#define TEX_HOOK(tpage, clut) (get_core_num() == 1 ? tex_hook(tpage, clut) : (void)0)

/* 8/15bpp sprite rows through the XIP stream engine (video.c) */
extern bool Video_TexFetch(const void *src, void *dst, uint32_t words);
extern void Video_TexWait(void);
/* the stream engine is core 1's (it also feeds the scanout) */
#define SOFT_ROW_FETCH(src, dst, words) (get_core_num() == 1 && Video_TexFetch(src, dst, words))
#define SOFT_ROW_WAIT() Video_TexWait()






#define SOFT_PER_CORE get_core_num()
/* SRAM proper is full: the row and palette buffers go to SCRATCH_Y */
#define SOFT_SCRATCH __attribute__((section(".scratch_y.soft")))
#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#define SOFT_CLUT_CACHE
/* core 0 reads palettes from PSRAM, or from the old copy of a palette row
 * core 1 has since overwritten (clut_old_for, below) */
static const u16 *clut_old_for(u16 id, const u16 *src, int n);
#define SOFT_CLUT_SIDE clut_old_for
/* big 4bpp triangles read their texture from an SRAM copy (per core) */
#define SOFT_TEX_SRAM_BYTES 4096
#define SOFT_RASTER_IRAM __attribute__((section(".time_critical.raster"), noinline))
#include "soft_raster.inc.c"

/* profiling: microseconds spent rasterizing / moving VRAM, read by video.c */
volatile uint32_t gpu_prim_us, gpu_vram_us, gpu_prims;

/* ================================================================== */
/* Asynchronous GPU: core 0 (game) queues, core 1 rasterizes.          */
/*                                                                    */
/* psyz walks each ordering table synchronously and calls Draw_* per   */
/* packet. Those calls only copy the packet into a ring; core 1 runs   */
/* the rasterizer from it (Gpu_Core1Work) while the game builds its    */
/* next frame. Ops that move pixel data in or out of VRAM, and the     */
/* VSync before a frame is shown, first wait for the ring to drain     */
/* (gpu_fence), so the cores never touch GPU state at the same time.   */
/* ================================================================== */

#define GQ_WORDS 8192 /* 32 KB; a busy SOTN frame queues ~5k words */
static uint32_t gq[GQ_WORDS] __attribute__((aligned(4)));
static volatile uint32_t gq_head, gq_tail; /* word counters, wrap modulo 2^32 */
static volatile bool gq_exec;             /* core 1 is executing a command */
volatile uint32_t gpu_fence_us;            /* core 0 time spent waiting */

enum { GQ_PRIM = 1, GQ_TPAGE, GQ_TWIN, GQ_AREA0, GQ_AREA1, GQ_OFFSET, GQ_MASK, GQ_CLEAR, GQ_MOVE, GQ_LOAD };
/* uploads up to this size are queued with their pixels (SOTN uploads the
 * player's sprite every frame); bigger ones wait for the queue to drain */
#define GQ_LOAD_MAX_WORDS 2048
static uint32_t load_buf[GQ_LOAD_MAX_WORDS + 2] __attribute__((aligned(4)));

/* timeline diagnostics: when core 0 queues a frame's first and last
 * command (us after VSync), and how long it waited on a full ring */
extern volatile uint32_t psxv_vsync_ret_us;
static uint32_t gq_first_at, gq_last_at;
volatile uint32_t gq_first_sum, gq_last_sum, gq_full_us;

/* hdr words, then payload words */
static void gq_push2(uint32_t type, const uint32_t *w, uint32_t n, const uint32_t *p, uint32_t pn) {
    uint32_t total = n + pn;
    uint32_t now = time_us_32() - psxv_vsync_ret_us;
    if (!gq_first_at) gq_first_at = now ? now : 1;
    if (GQ_WORDS - (gq_head - gq_tail) < total + 1) {
        uint32_t t0 = time_us_32();
        while (GQ_WORDS - (gq_head - gq_tail) < total + 1) tight_loop_contents(); /* ring full */
        gq_full_us += time_us_32() - t0;
    }
    gq_last_at = time_us_32() - psxv_vsync_ret_us;
    uint32_t h = gq_head;
    gq[h & (GQ_WORDS - 1)] = type << 24 | total;
    for (uint32_t i = 0; i < n; i++) gq[(h + 1 + i) & (GQ_WORDS - 1)] = w[i];
    for (uint32_t i = 0; i < pn; i++) gq[(h + 1 + n + i) & (GQ_WORDS - 1)] = p[i];
    __dmb();
    gq_head = h + 1 + total;
}
static inline void gq_push(uint32_t type, const uint32_t *w, uint32_t n) { gq_push2(type, w, n, NULL, 0); }

volatile uint32_t gpu_fence_by[6]; /* us waited: sync, display, load, store, reset, other */
static void gpu_fence_c(int why) {
    if (gq_head == gq_tail && !gq_exec) return;
    uint64_t t = time_us_64();
    while (gq_head != gq_tail || gq_exec) tight_loop_contents();
    __dmb();
    uint32_t dt = (uint32_t)(time_us_64() - t);
    gpu_fence_us += dt;
    gpu_fence_by[why] += dt;
}
#define gpu_fence() gpu_fence_c(5)

/* Words a GP0 primitive occupies -- the same accounting as soft_push_prim,
 * which the caller needs right away to walk the packet stream. */
static int prim_len(const u_long *p, int max_len) {
    int code = (int)(p[0] >> 24) & 0xFF;
    bool textured = (code & TEXTURED) != 0, gouraud = (code & GOURAUD) != 0;
    int n;
    if (!(code & 0x40)) { /* polygon */
        if (!(code & TRIANGLE)) return 1;
        int v = (code & EXTRA_VERTEX) ? 4 : 3;
        n = 1 + v * (1 + textured) + (gouraud ? v - 1 : 0);
    } else if (!(code & 0x20)) { /* line */
        int pts = ((code >> 2) & 3) + 1;
        bool padding = true;
        if (pts == 1) {
            padding = false;
            pts = 2;
        }
        n = 1 + pts + (gouraud ? pts - 1 : 0) + padding;
    } else { /* rectangle */
        n = 2 + textured + ((code & 0x18) == 0 ? 1 : 0);
    }
    return n < max_len ? n : max_len;
}

/* VRAM upload: rows go straight to PSRAM through the uncached alias (a
 * cached store costs a write-allocate per 8-byte line); the cached copy of
 * each row is cleaned and invalidated first. */
static void load_image_fast(const PS1_RECT *r, const u16 *src) {
    if (!src) return;
    clut_cache_flush();
    int w = r->w, h = r->h;
    if (r->x < 0 || r->y < 0 || r->x + w > VRAM_W || w <= 0 || h <= 0) {
        soft_load_image(r, src);
        vrow_forget(0, 0, VRAM_W, VRAM_H);
        return;
    }
    vrow_forget(r->x, r->y, w, h);
    for (int j = 0; j < h; j++) {
        u16 *row = g_PsxVram + ((r->y + j) & (VRAM_H - 1)) * VRAM_PITCH + r->x;
        row_cache_sync(row, w, true);
        copy_px((u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), src, w);
        src += w;
    }
}

/* ---- core 0 helps rasterize while it waits in DrawSync ----
 *
 * SOTN queues a whole frame early, then core 0 sits in DrawSync for most of
 * the frame while core 1 draws. While core 0 waits it takes the odd rows of
 * every primitive: core 1 runs each command as before but draws only even
 * rows, then publishes the command; core 0 follows, drawing the odd rows
 * with its own rasterizer state. Within each core primitives keep their
 * order, and a semi-transparent pixel only reads itself, so the picture is
 * the same.
 *
 * Anything that changes slots (allocation, load, flush) or moves VRAM first
 * waits for core 0 to catch up (help_sync) and is done by core 1 alone. A
 * clear or upload waits only for the last of core 0's unfinished commands
 * that reads or draws the area (fp_*, slot_last); a palette row still in
 * use is copied for core 0 instead (clut_old), and other writes still in
 * use are handed to core 0, which does them in order (help_defer). Core 1
 * mixes audio while it waits. Core 0 only helps inside DrawSync, when
 * nothing new is queued, so the ring words it reads stay valid. */
#define HELP_N 256
/* ring position (low 13 bits, GQ_WORDS), type GQ_* (bits 13-16), target
 * slot + 1 (bits 17-19; 0: PSRAM) */
typedef uint32_t HelpCmd;
static HelpCmd help_q[HELP_N] __attribute__((section(".scratch_y.help")));
static volatile uint32_t help_pub;  /* commands published by core 1 */
static volatile uint32_t help_cur;  /* command core 0 is running */

/* Palette rows overwritten while core 0 still had to draw commands that use
 * them: core 1 keeps the old row here instead of waiting (palette cycling
 * uploads a CLUT row every frame, right after the primitives using it). */
#define CLUT_OLD_N 4
typedef struct {
    int16_t x, y, w;
    uint32_t until; /* commands before this one see the old row */
    u16 px[256];
} ClutOld;
static ClutOld clut_old[CLUT_OLD_N];
static volatile uint32_t clut_old_used;
volatile uint32_t clut_old_saves; /* perf log */

static const u16 *clut_old_for(u16 id, const u16 *src, int n) {
    uint32_t used = clut_old_used;
    if (!used) return src;
    int cx = (id & 0x3F) * 16, cy = (id >> 6) & 0x1FF;
    for (; used; used &= used - 1) {
        ClutOld *e = &clut_old[__builtin_ctz(used)];
        if (cy == e->y && cx >= e->x && cx + n <= e->x + e->w && (int32_t)(help_cur - e->until) < 0)
            return e->px + (cx - e->x);
    }
    return src;
}
static volatile uint32_t help_done; /* commands core 0 has finished */
static volatile bool help_req;      /* core 0 is in DrawSync and can help */
static volatile bool help_on;       /* split active (set by core 1, cleared by core 0) */
volatile uint32_t help_cmds, help_frames; /* diagnostic */
/* perf log: core 1 waiting for core 0 (sync, full ring), core 0 drawing,
 * and when the split started (us after VSync returned) */
volatile uint32_t help_wait_sync_us, help_wait_full_us, help_draw_us, help_start_us, help_idle_us;
/* the last of these is a lag: how long core 0 went on after core 1's last command */
static volatile uint32_t help_c1_wait;
static volatile uint32_t help_c1_last_us; /* when core 1 last published */
volatile uint32_t help_lag_us;

extern bool Audio_Core1Poll(void);
/* perf log, by site: flush, slot load, slot alloc, clear, move, upload */
/* the last: a primitive waiting for a write handed to core 0 */
volatile uint32_t help_sync_n[7], help_sync_us[7];
static void help_sync(int site) {
    if (!help_on || get_core_num() != 1) return;
    help_sync_n[site]++;
    if (help_done == help_pub) return;
    uint32_t t = time_us_32();
    while (help_done != help_pub)
        if (!Audio_Core1Poll()) tight_loop_contents(); /* mix while core 0 catches up */
    help_sync_us[site] += time_us_32() - t;
    help_wait_sync_us += time_us_32() - t;
    help_c1_wait += time_us_32() - t;
}

/* half = 0: even rows (core 1), 1: odd rows (core 0), -1: all of them.
 * Rows rather than halves: a frame's heavy primitives are often in one part
 * of the screen (lava along the bottom), and every row has its own pixels,
 * so either core's share is the same size wherever the work is. */
static void push_band(u_long *packets, int len, int half) {
    soft_par = half;
    soft_push_prim(packets, len);
    soft_par = -1;
}

/* What core 0's unfinished commands read or write in VRAM: texture and
 * CLUT rectangles and draw areas, kept by core 1 as it publishes and
 * forgotten whenever core 0 has caught up. An upload that misses all of
 * them need not wait for core 0. */
#define FP_N 128
static int16_t fp[FP_N][4]; /* texture, CLUT and PSRAM draw rectangles: x0, y0, x1, y1 inclusive */
static uint32_t fp_last[FP_N]; /* help_done value once its last user is drawn */
static uint8_t fp_clut[FP_N];  /* a palette (core 0 can be given its old copy) */
static int fp_n;

volatile uint32_t help_upload_free; /* perf log: uploads that did not wait */

/* Clears and uploads handed to core 0 (help_defer): their rectangle and
 * command position. Done once help_done is past it. */
#define PW_N 8
static int16_t pw[PW_N][4];
static uint32_t pw_idx[PW_N];
static int pw_n;
static bool pw_checking; /* fp_add measures against pw instead of recording */
static uint32_t pw_need;
volatile uint32_t help_deferred; /* perf log */

static void pw_prune(void) {
    int j = 0;
    for (int i = 0; i < pw_n; i++)
        if ((int32_t)(help_done - pw_idx[i]) <= 0) {
            for (int k = 0; k < 4; k++) pw[j][k] = pw[i][k];
            pw_idx[j++] = pw_idx[i];
        }
    pw_n = j;
}

/* help_done value once every handed-over write overlapping the box is done */
static uint32_t pw_need_box(int x0, int y0, int x1, int y1) {
    uint32_t need = 0;
    for (int i = 0; i < pw_n; i++)
        if (x0 <= pw[i][2] && x1 >= pw[i][0] && y0 <= pw[i][3] && y1 >= pw[i][1] &&
            (int32_t)(pw_idx[i] + 1 - need) > 0)
            need = pw_idx[i] + 1;
    return need;
}

static void fp_add(int x0, int y0, int x1, int y1, int clut) {
    if (pw_checking) {
        uint32_t n = pw_need_box(x0, y0, x1, y1);
        if ((int32_t)(n - pw_need) > 0) pw_need = n;
        return;
    }
    for (int i = 0; i < fp_n; i++)
        if (fp[i][0] <= x0 && fp[i][1] <= y0 && fp[i][2] >= x1 && fp[i][3] >= y1 && fp_clut[i] == clut) {
            fp_last[i] = help_pub + 1;
            return;
        }
    int i = fp_n;
    if (fp_n < FP_N) {
        fp_n++;
    } else { /* full: grow the rectangle that grows least (one giant
              * rectangle would make every upload wait) */
        int best = INT32_MAX;
        for (int k = 0; k < FP_N; k++) {
            if (fp_clut[k] != clut) continue;
            int a0 = (fp[k][2] - fp[k][0] + 1) * (fp[k][3] - fp[k][1] + 1);
            int nx0 = x0 < fp[k][0] ? x0 : fp[k][0], ny0 = y0 < fp[k][1] ? y0 : fp[k][1];
            int nx1 = x1 > fp[k][2] ? x1 : fp[k][2], ny1 = y1 > fp[k][3] ? y1 : fp[k][3];
            int g = (nx1 - nx0 + 1) * (ny1 - ny0 + 1) - a0;
            if (g < best) best = g, i = k;
        }
        if (x0 > fp[i][0]) x0 = fp[i][0];
        if (y0 > fp[i][1]) y0 = fp[i][1];
        if (x1 < fp[i][2]) x1 = fp[i][2];
        if (y1 < fp[i][3]) y1 = fp[i][3];
    }
    fp[i][0] = (int16_t)x0;
    fp[i][1] = (int16_t)y0;
    fp[i][2] = (int16_t)x1;
    fp[i][3] = (int16_t)y1;
    fp_last[i] = help_pub + 1;
    fp_clut[i] = (uint8_t)clut;
}

volatile int16_t fp_hit_log[4][8]; /* upload rect and the rect it hit (perf log) */
volatile uint32_t fp_hit_n;
/* how far core 0 must get (help_done) before r may be overwritten: the
 * last unfinished command that reads or draws there; 0 if none does */
static uint32_t fp_need(const PS1_RECT *r, bool *clut_only) {
    uint32_t need = 0;
    *clut_only = true; /* every pending hit is a one-row palette */
    for (int i = 0; i < fp_n; i++) {
        if (r->x <= fp[i][2] && r->x + r->w - 1 >= fp[i][0] && r->y <= fp[i][3] && r->y + r->h - 1 >= fp[i][1] &&
            (int32_t)(fp_last[i] - help_done) > 0 && !fp_clut[i])
            *clut_only = false;
        if (r->x <= fp[i][2] && r->x + r->w - 1 >= fp[i][0] && r->y <= fp[i][3] && r->y + r->h - 1 >= fp[i][1] &&
            fp_last[i] > need) {
            need = fp_last[i];
            volatile int16_t *l = fp_hit_log[fp_hit_n++ & 3];
            l[0] = r->x; l[1] = r->y; l[2] = r->w; l[3] = r->h;
            l[4] = fp[i][0]; l[5] = fp[i][1]; l[6] = fp[i][2]; l[7] = fp[i][3];
        }
    }
    return need;
}

/* core 1, before uploading over a palette row core 0 still needs: keep the
 * old row for core 0's commands before this one. False: no room, wait. */
static bool clut_old_save(const PS1_RECT *r) {
    if (r->h != 1 || r->w > 256 || r->w <= 0 || r->x < 0 || r->x + r->w > VRAM_W) return false;
    for (int i = 0; i < NSLOT; i++)
        if (overlaps(&slot[i], r->x, r->y, r->w, 1)) return false; /* row lives in SRAM */
    for (int i = 0; i < CLUT_OLD_N; i++) {
        ClutOld *e = &clut_old[i];
        if ((clut_old_used & (1u << i)) && (int32_t)(help_done - e->until) < 0) continue;
        clut_old_used &= ~(1u << i);
        __dmb();
        e->x = (int16_t)r->x;
        e->y = (int16_t)r->y;
        e->w = (int16_t)r->w;
        e->until = help_pub; /* this upload's own position */
        const u16 *row = g_PsxVram + (r->y & (VRAM_H - 1)) * VRAM_PITCH + r->x;
        for (int k = 0; k < r->w; k++) e->px[k] = row[k];
        __dmb();
        clut_old_used |= 1u << i;
        return true;
    }
    return false;
}

/* core 1, before an upload or clear of r while core 0 helps: wait only
 * until core 0 is past the last command that uses the area. A palette row
 * still in use is copied for core 0 instead (palette cycling uploads a CLUT
 * row every frame right after the primitives that use it). */
static uint32_t slot_last[NSLOT]; /* help_done value once core 0's last primitive into the slot is drawn */
volatile int16_t wait_log[4][8];
volatile uint32_t wait_log_n;
static int slot_c0_top[NSLOT];    /* highest row core 0 writes there: the top of its areas */

/* core 1: spin (mixing audio) until core 0 is that far */
static void help_wait_done(uint32_t need, int site) {
    if ((int32_t)(need - help_done) <= 0) return;
    help_sync_n[site]++;
    uint32_t t0 = time_us_32();
    while ((int32_t)(need - help_done) > 0)
        if (!Audio_Core1Poll()) tight_loop_contents();
    help_sync_us[site] += time_us_32() - t0;
    help_wait_sync_us += time_us_32() - t0;
    help_c1_wait += time_us_32() - t0;
}

/* core 1, at a clear or upload of r that core 0's unfinished commands still
 * read: hand it to core 0 instead of waiting. Core 0 runs the commands in
 * order, so when it gets to the write everything before it is drawn; core 1
 * goes on and waits only if it touches r first (help_pw_wait). Not for
 * framebuffer slots, nor uploads whose data wraps around the ring. */
static bool help_defer(const PS1_RECT *r, uint32_t pos, uint32_t type) {
    if (pw_n) pw_prune();
    if (pw_n >= PW_N || r->w <= 0 || r->h <= 0 || r->x < 0 || r->y < 0 || r->x + r->w > VRAM_W ||
        r->y + r->h > VRAM_H)
        return false;
    for (int k = 0; k < NSLOT; k++)
        if (overlaps(&slot[k], r->x, r->y, r->w, r->h)) return false;
    if (type == GQ_LOAD && ((pos + 1) & (GQ_WORDS - 1)) + (gq[pos & (GQ_WORDS - 1)] & 0xFFFFFF) > GQ_WORDS)
        return false;
    bool clut_only;
    uint32_t need = fp_need(r, &clut_only);
    uint32_t pn = pw_need_box(r->x, r->y, r->x + r->w - 1, r->y + r->h - 1);
    if ((int32_t)(pn - need) > 0) need = pn;
    if (!need || (int32_t)(need - help_done) <= 0) return false; /* nobody waits: do it now */
    pw[pw_n][0] = (int16_t)r->x;
    pw[pw_n][1] = (int16_t)r->y;
    pw[pw_n][2] = (int16_t)(r->x + r->w - 1);
    pw[pw_n][3] = (int16_t)(r->y + r->h - 1);
    pw_idx[pw_n++] = help_pub; /* this command's position */
    clut_cache_flush(); /* cached palettes may come from r */
    help_deferred++;
    return true;
}

/* core 1, before a primitive while writes are with core 0: wait for those
 * its texture, palette or (PSRAM) draw area overlap */
static void fp_add_prim(const uint32_t *w);
static void help_pw_wait(const uint32_t *w) {
    pw_prune();
    if (!pw_n) return;
    pw_checking = true;
    pw_need = 0;
    fp_add_prim(w);
    fp_add(st.x0, st.y0, st.x1, st.y1, 0);
    pw_checking = false;
    help_wait_done(pw_need, 6);
}

static void help_wait_for(const PS1_RECT *r, int site) {
    bool clut_only;
    uint32_t need = fp_need(r, &clut_only);
    if (pw_n) {
        pw_prune();
        uint32_t pn = pw_need_box(r->x, r->y, r->x + r->w - 1, r->y + r->h - 1);
        if ((int32_t)(pn - need) > 0) need = pn, clut_only = false;
    }
    for (int k = 0; k < NSLOT; k++) /* a framebuffer slot: its last primitive */
        if (overlaps(&slot[k], r->x, r->y, r->w, r->h) && r->y + r->h > slot_c0_top[k] &&
            (int32_t)(slot_last[k] - need) > 0) {
            need = slot_last[k];
            clut_only = false;
        }
    if ((int32_t)(need - help_pub) > 0) need = help_pub;
    if (!need || (int32_t)(need - help_done) <= 0) {
        help_upload_free++;
        return;
    }
    if (clut_only && clut_old_save(r)) {
        clut_old_saves++;
        return;
    }
    help_sync_n[site]++;
    {
        volatile int16_t *l = wait_log[wait_log_n++ & 3];
        l[0] = (int16_t)site; l[1] = r->x; l[2] = r->y; l[3] = r->w; l[4] = r->h;
        l[5] = (int16_t)(need - help_done); l[6] = (int16_t)slot_c0_top[0]; l[7] = (int16_t)slot_c0_top[1];
    }
    uint32_t t0 = time_us_32();
    while ((int32_t)(need - help_done) > 0)
        if (!Audio_Core1Poll()) tight_loop_contents();
    help_sync_us[site] += time_us_32() - t0;
    help_wait_sync_us += time_us_32() - t0;
    help_c1_wait += time_us_32() - t0;
}

/* texels u0..u1, v0..v1 of tpage, and the CLUT if paletted */
static void fp_add_tex(uint32_t tpage, uint32_t clut, int u0, int v0, int u1, int v1) {
    int px = (int)(tpage & 0xF) * 64, py = (int)((tpage >> 4) & 1) * 256, bpp = (int)(tpage >> 7) & 3;
    if (u1 > 255 || v1 > 255) u0 = v0 = 0, u1 = v1 = 255; /* wraps */
    int sh = bpp == 0 ? 2 : bpp == 1 ? 1 : 0;
    fp_add(px + (u0 >> sh), py + v0, px + (u1 >> sh), py + v1, 0);
    if (bpp < 2) {
        int cx = (int)(clut & 0x3F) * 16, cy = (int)(clut >> 6) & 511;
        fp_add(cx, cy, cx + (bpp ? 255 : 15), cy, 1);
    }
}

static void fp_add_prim(const uint32_t *w) {
    int code = (int)(w[0] >> 24) & 0xFF;
    if (!(code & TEXTURED)) return;
    if (!(code & 0x40)) { /* polygon */
        int n = (code & EXTRA_VERTEX) ? 4 : 3, k = 1, u0 = 255, v0 = 255, u1 = 0, v1 = 0;
        uint32_t clut = 0, tpage = 0;
        for (int i = 0; i < n; i++) {
            if (i > 0 && (code & GOURAUD)) k++;
            uint32_t uv = w[k + 1];
            int u = (int)(uv & 0xFF), v = (int)((uv >> 8) & 0xFF);
            if (i == 0) clut = uv >> 16;
            if (i == 1) tpage = uv >> 16;
            if (u < u0) u0 = u;
            if (u > u1) u1 = u;
            if (v < v0) v0 = v;
            if (v > v1) v1 = v;
            k += 2;
        }
        /* the fill rule leaves the right and bottom edges out: a polygon
         * mapping texels u0..u1 stops sampling at u1 - 1 (a rotated one can
         * touch the last row at a corner: one row, one frame) */
        if (u1 > u0) u1--;
        if (v1 > v0) v1--;
        fp_add_tex(tpage, clut, u0, v0, u1, v1);
    } else if (code & 0x20) { /* sprite */
        int u = (int)(w[2] & 0xFF), v = (int)((w[2] >> 8) & 0xFF), sw, sh;
        switch (code & 0x18) {
        case 0x00: sw = (int16_t)w[3], sh = (int16_t)(w[3] >> 16); break;
        case 0x08: sw = sh = 1; break;
        case 0x10: sw = sh = 8; break;
        default: sw = sh = 16; break;
        }
        if (sw <= 0 || sh <= 0) return;
        fp_add_tex(soft_cur_tpage[1], w[2] >> 16, u, v, u + sw - 1, v + sh - 1);
    }
}

/* core 1, after running command at ring position pos */
static void help_publish(uint32_t pos, uint32_t type, int slot_i, bool mine) {
    if (help_done == help_pub) { /* core 0 has caught up */
        fp_n = 0;
    }
    if (type == GQ_PRIM) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = gq[(pos + 1 + i) & (GQ_WORDS - 1)];
        fp_add_prim(w);
        /* drawing into PSRAM: the draw area counts too (into a slot it does
         * not: an upload over a slot flushes it, and that waits for core 0) */
        if (slot_i < 0) fp_add(st.x0, st.y0, st.x1, st.y1, 0);
        else {
            slot_last[slot_i] = help_pub + 1;
            if (st.y0 < slot_c0_top[slot_i]) slot_c0_top[slot_i] = st.y0; /* its rows are everywhere */
        }
    }
    if (help_pub - help_done >= HELP_N) {
        uint32_t t = time_us_32();
        while (help_pub - help_done >= HELP_N)
            if (!Audio_Core1Poll()) tight_loop_contents();
        help_wait_full_us += time_us_32() - t;
        help_c1_wait += time_us_32() - t;
    }
    help_q[help_pub % HELP_N] = (pos & (GQ_WORDS - 1)) | (type << 13) | ((uint32_t)(slot_i + 1) << 17) |
                                ((uint32_t)mine << 20);
    help_c1_last_us = time_us_32();
    __dmb();
    help_pub = help_pub + 1;
}

/* core 0: a clear or upload core 1 handed over. Same writes as exec_clear's
 * PSRAM path and load_image_fast, minus the palette cache (core 1's, which
 * flushed it when it handed the write over). The upload's data is still in
 * the ring, unwrapped (help_defer checked). */
static void help_exec_write(uint32_t pos, uint32_t type) {
    const uint32_t *wp = &gq[(pos + 1) & (GQ_WORDS - 1)];
    const PS1_RECT *r = (const PS1_RECT *)wp;
    if (type == GQ_CLEAR) {
        u16 c = (u16)((((u_char)wp[2]) >> 3) | ((((u_char)wp[3]) >> 3) << 5) | ((((u_char)wp[4]) >> 3) << 10));
        for (int j = 0; j < r->h; j++) {
            u16 *row = g_PsxVram + (r->y + j) * VRAM_PITCH + r->x;
            for (int k = 0; k < r->w; k++) row[k] = c;
        }
    } else {
        const u16 *src = (const u16 *)&wp[2];
        for (int j = 0; j < r->h; j++) {
            u16 *row = g_PsxVram + (r->y + j) * VRAM_PITCH + r->x;
            row_cache_sync(row, r->w, true);
            copy_px((u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), src, r->w);
            src += r->w;
        }
    }
    vrow_forget(r->x, r->y, r->w, r->h);
}

/* core 0: run one published command on its own rasterizer */
static void help_exec(HelpCmd c) {
    uint32_t pos = c & (GQ_WORDS - 1), type = (c >> 13) & 15;
    int slot_i = (int)(c >> 17) - 1;
    uint32_t hdr = gq[pos];
    uint32_t n = hdr & 0xFFFFFF;
    uint32_t w[64];
    if (type > GQ_MASK) {
        if (c >> 20 & 1) help_exec_write(pos, type); /* handed over (help_defer) */
        return;                                      /* else done by core 1 */
    }
    for (uint32_t i = 0; i < n && i < 64; i++) w[i] = gq[(pos + 1 + i) & (GQ_WORDS - 1)];
    switch (type) {
    case GQ_PRIM: {
        uint32_t t = time_us_32();
        draw_into(slot_i);
        push_band((u_long *)w, (int)n, 1);
        help_draw_us += time_us_32() - t;
        help_cmds++;
        break;
    }
    case GQ_TPAGE: soft_set_texpage_mode((ParamDrawTexpageMode *)&w[0]); break;
    case GQ_TWIN: soft_set_texture_window(w[0], w[1], w[2], w[3]); break;
    case GQ_AREA0: soft_set_area_start((int)w[0], (int)w[1]); break;
    case GQ_AREA1: soft_set_area_end((int)w[0], (int)w[1]); break;
    case GQ_OFFSET: soft_set_offset((int)w[0], (int)w[1]); break;
    case GQ_MASK: soft_set_mask((int)w[0], (int)w[1]); break;
    }
}

/* core 0, in DrawSync: help until core 1 has drained the ring */
extern bool Audio_HelpIdleFill(void);
static void gpu_help(void) {
    help_req = true;
    __dmb();
    uint32_t k = 0, idle = 0;
    bool helped = false;
    for (;;) {
        if (help_on) {
            helped = true;
            if (k != help_pub) {
                help_cur = k;
                help_exec(help_q[k % HELP_N]);
                k++;
                __dmb();
                help_done = k;
                continue;
            }
            /* core 1 publishes before it advances the tail */
            if (gq_head == gq_tail && !gq_exec && k == help_pub) break;
            uint32_t t = time_us_32();
            bool mixed = Audio_HelpIdleFill(); /* nothing to draw: mix */
            idle += time_us_32() - t;
            if (mixed) continue;
            t = time_us_32();
            tight_loop_contents();
            idle += time_us_32() - t;
            continue;
        } else if (gq_head == gq_tail && !gq_exec) {
            break; /* core 1 accepts only while it runs a command */
        }
        tight_loop_contents();
    }
    help_req = false;
    __dmb();
    help_on = false;
    if (helped) {
        help_frames++;
        help_idle_us += idle;
        help_lag_us += time_us_32() - help_c1_last_us;
    }
}

/* ---- core 1: execution ---- */

/* poly flat, poly tex, poly tex+gouraud, line, sprite, tile; +6 semi */
volatile uint32_t gpu_class_us[12], gpu_class_n[12], gpu_class_px[12];
volatile uint32_t gpu_psram_prims; /* perf log: primitives drawn straight into PSRAM, and the last area */
volatile int16_t gpu_psram_area[4];
static void exec_prim(u_long *packets, int len) {
    uint64_t t = time_us_64();
    gpu_c1_state = 1;
    int i = slot_for(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1);
    if (i >= 0) {
        if (!slot[i].loaded) slot_load(&slot[i]);
        slot[i].dirty = true;
    }
    draw_into(i);
    push_band(packets, len, help_on ? 0 : -1);
    if (i < 0) { /* drew into PSRAM */
        vrow_forget(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1);
        gpu_psram_prims++;
        gpu_psram_area[0] = (int16_t)st.x0, gpu_psram_area[1] = (int16_t)st.y0;
        gpu_psram_area[2] = (int16_t)st.x1, gpu_psram_area[3] = (int16_t)st.y1;
    }
    uint32_t dt = (uint32_t)(time_us_64() - t);
    gpu_prim_us += dt;
    frame_raster_us += dt;
    gpu_prims++;
    {
        /* time by primitive class (perf log) */
        int code = (int)(packets[0] >> 24) & 0xFF, k;
        if (!(code & 0x40)) k = (code & TEXTURED) ? ((code & GOURAUD) ? 2 : 1) : 0;
        else if (!(code & 0x20)) k = 3;
        else k = (code & TEXTURED) ? 4 : 5;
        if (code & SEMITRANSP) k += 6;
        gpu_class_us[k] += dt;
        gpu_class_n[k]++;
        gpu_class_px[k] += soft_px;
        soft_px = 0;
    }
}

/* slowest VRAM op this frame (diagnostic): kind 1 load, 2 move, 3 store */
volatile uint32_t big_op_kind, big_op_w, big_op_h, big_op_us;
#define TIMED_RECT(kind, rect, call)                                  \
    do {                                                              \
        uint64_t t0_ = time_us_64();                                  \
        gpu_c1_state = 5;                                             \
        call;                                                         \
        uint32_t d_ = (uint32_t)(time_us_64() - t0_);                 \
        gpu_vram_us += d_;                                            \
        if (d_ > big_op_us) {                                         \
            big_op_us = d_;                                           \
            big_op_kind = (kind);                                     \
            big_op_w = (rect)->w;                                     \
            big_op_h = (rect)->h;                                     \
        }                                                             \
    } while (0)
#define TIMED(call)                                                   \
    do {                                                              \
        uint64_t t0_ = time_us_64();                                  \
        gpu_c1_state = 5;                                             \
        call;                                                         \
        gpu_vram_us += (uint32_t)(time_us_64() - t0_);                \
    } while (0)

/* A VRAM op reads rect in PSRAM: publish overlapping slots first. */
static void before_read(const PS1_RECT *r) { flush_overlapping(r->x, r->y, r->w, r->h); }

/* A VRAM op writes rect in PSRAM: overlapping slots must reload it. */
static void before_write(const PS1_RECT *r) {
    for (int i = 0; i < NSLOT; i++)
        if (overlaps(&slot[i], r->x, r->y, r->w, r->h)) {
            slot_flush_c(&slot[i], CAUSE_WRITE);
            slot[i].loaded = false;
        }
}

static void exec_clear(PS1_RECT *rect, u_char r, u_char g, u_char b) {
    uint64_t t0 = time_us_64();
    gpu_c1_state = 4;
    /* clearing a whole framebuffer (a slot's rectangle, or the draw area):
     * fill its slot, no load needed */
    int i = -1;
    for (int k = 0; k < NSLOT; k++)
        if (slot[k].used && slot[k].x0 == rect->x && slot[k].y0 == rect->y &&
            slot[k].w == rect->w && slot[k].h == rect->h)
            i = k;
    if (i < 0 && rect->x == st.x0 && rect->y == st.y0 && rect->w == st.x1 - st.x0 + 1 &&
        rect->h == st.y1 - st.y0 + 1)
        i = slot_for(rect->x, rect->y, rect->w, rect->h);
    if (i >= 0) {
        Slot *s = &slot[i];
        u16 c = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
        uint32_t c2 = c | ((uint32_t)c << 16);
        for (int j = 0; j < s->h; j++) {
            scan_poll(); /* a 512x240 clear is long enough to starve the scanout */
            u16 *row = s->base + j * s->sw;
            int n = s->w;
            if (((uintptr_t)row & 2) && n) {
                *row++ = c;
                n--;
            }
            uint32_t *q = (uint32_t *)row;
            for (int k = 0; k < n / 2; k++) q[k] = c2;
            if (n & 1) row[n - 1] = c;
        }
        s->loaded = true;
        s->dirty = true;
        clut_cache_flush();
    } else {
        before_write(rect);
        soft_clear_image(rect, r, g, b);
        vrow_forget(rect->x, rect->y, rect->w, rect->h);
    }
    gpu_vram_us += (uint32_t)(time_us_64() - t0);
}

static void exec_move(PS1_RECT *rect, int x, int y) {
    PS1_RECT dst = {(short)x, (short)y, rect->w, rect->h};
    before_read(rect);
    before_write(&dst);
    TIMED_RECT(2, rect, soft_move_image(rect, x, y));
    vrow_forget(x, y, rect->w, rect->h);
}

/* frame timeline (perf log): offsets from the last VSync return */
volatile uint32_t gpu_sync_at_us, gpu_drain_at_us, gpu_tl_frames;
volatile uint32_t gpu_last_sync_at, gpu_last_drain_at; /* this frame: DrawSync returned / core 1 drained */
static volatile uint32_t drain_last;
extern volatile uint32_t psxv_vsync_ret_us;

void Gpu_Core1FlushPending(void);

/* Run queued commands for up to budget_us. Returns true if it did work. */
volatile uint32_t gpu_busy_us; /* core 1 time in Gpu_Core1Work (diagnostic) */
bool __not_in_flash_func(Gpu_Core1Work)(uint32_t budget_us) {
    if (fl_core1) Gpu_Core1FlushPending();
    if (gq_head == gq_tail) return false;
    uint32_t t0 = time_us_32();
    static uint32_t w[64];
    do {
        gq_exec = true;
        __dmb();
        if (help_req && !help_on) {
            /* core 0 is waiting in DrawSync: split from here on, its
             * rasterizer starting from core 1's state */
            soft_st[0] = soft_st[1];
            soft_cur_tpage[0] = soft_cur_tpage[1];
            help_pub = help_done = 0;
            fp_n = 0; /* nothing handed over yet */
            pw_n = 0;
            for (int k = 0; k < NSLOT; k++) slot_last[k] = 0, slot_c0_top[k] = VRAM_H * 2;
            clut_old_used = 0;
                help_c1_wait = 0;
            help_start_us += time_us_32() - psxv_vsync_ret_us;
            __dmb();
            help_on = true;
        }
        uint32_t t = gq_tail;
        uint32_t hdr = gq[t & (GQ_WORDS - 1)];
        uint32_t type = hdr >> 24, n = hdr & 0xFFFFFF;
        bool deferred = false; /* a clear or upload left to core 0 */
        if (type == GQ_LOAD) {
            for (uint32_t i = 0; i < n; i++) load_buf[i] = gq[(t + 1 + i) & (GQ_WORDS - 1)];
        } else {
            for (uint32_t i = 0; i < n && i < 64; i++) w[i] = gq[(t + 1 + i) & (GQ_WORDS - 1)];
        }
        switch (type) {
        case GQ_PRIM:
            if (pw_n) help_pw_wait(w);
            exec_prim((u_long *)w, (int)n);
            break;
        case GQ_TPAGE: soft_set_texpage_mode((ParamDrawTexpageMode *)&w[0]); break;
        case GQ_TWIN: soft_set_texture_window(w[0], w[1], w[2], w[3]); break;
        case GQ_AREA0: soft_set_area_start((int)w[0], (int)w[1]); break;
        case GQ_AREA1: soft_set_area_end((int)w[0], (int)w[1]); break;
        case GQ_OFFSET: soft_set_offset((int)w[0], (int)w[1]); break;
        case GQ_MASK: soft_set_mask((int)w[0], (int)w[1]); break;
        case GQ_CLEAR: {
            PS1_RECT *cr = (PS1_RECT *)&w[0];
            /* core 0 does it if its unfinished commands still use the area;
             * otherwise wait for the last of them that draw into or read it
             * (a new slot for it waits in slot_for) */
            if (help_on && help_defer(cr, t, GQ_CLEAR)) {
                deferred = true;
                break;
            }
            if (!help_on) help_sync(3);
            else help_wait_for(cr, 3);
            exec_clear(cr, (u_char)w[2], (u_char)w[3], (u_char)w[4]);
            break;
        }
        case GQ_MOVE:
            help_sync(4);
            exec_move((PS1_RECT *)&w[0], (int)w[2], (int)w[3]);
            break;
        case GQ_LOAD: {
            PS1_RECT *r = (PS1_RECT *)&load_buf[0];
            if (help_on && help_defer(r, t, GQ_LOAD)) {
                deferred = true;
                break;
            }
            if (help_on) help_wait_for(r, 5);
            else help_sync(5);
            before_write(r);
            TIMED_RECT(1, r, load_image_fast(r, (const u16 *)&load_buf[2]));
            break;
        }
        }
        if (help_on) help_publish(t, type, draw_cur_c[1], deferred);
        __dmb();
        gq_tail = t + 1 + n;
        gq_exec = false;
    } while (gq_head != gq_tail && time_us_32() - t0 < budget_us);
    gpu_c1_state = 0;
    if (gq_head == gq_tail) drain_last = time_us_32() - psxv_vsync_ret_us;
    gpu_busy_us += time_us_32() - t0;
    return true;
}

/* ---- core 0: the psyz Draw_* backend ---- */

void Draw_Reset(void) {
    gpu_fence_c(4);
    vrow_forget(0, 0, VRAM_W, VRAM_H);
    slot[0].used = slot[1].used = slot[2].used = false;
    /* runs on core 0, resets core 1's rasterizer */
    draw_cur_c[1] = -1;
    clut_cache_flush();
    soft_st[1] = (SoftState){0, 0, VRAM_W - 1, VRAM_H - 1, 0, 0};
    soft_cur_tpage[1] = 0;
}

int Draw_SetHorizontalGrid(unsigned int source_width, unsigned int target_width) {
    (void)source_width;
    (void)target_width;
    return 0;
}

int Draw_PushPrim(u_long *packets, int max_len) {
    int n = prim_len(packets, max_len);
    if (n > 64) n = 64;
    gq_push(GQ_PRIM, (const uint32_t *)packets, (uint32_t)n);
    return n;
}

void Draw_SetTexpageMode(ParamDrawTexpageMode *p) {
    uint32_t w = *(uint32_t *)p;
    gq_push(GQ_TPAGE, &w, 1);
}
void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y, unsigned int off_x,
                           unsigned int off_y) {
    uint32_t w[4] = {mask_x, mask_y, off_x, off_y};
    gq_push(GQ_TWIN, w, 4);
}
void Draw_SetAreaStart(int x, int y) {
    uint32_t w[2] = {(uint32_t)x, (uint32_t)y};
    gq_push(GQ_AREA0, w, 2);
}
void Draw_SetAreaEnd(int x, int y) {
    uint32_t w[2] = {(uint32_t)x, (uint32_t)y};
    gq_push(GQ_AREA1, w, 2);
}
void Draw_SetOffset(int x, int y) {
    uint32_t w[2] = {(uint32_t)x, (uint32_t)y};
    gq_push(GQ_OFFSET, w, 2);
}
void Draw_SetMask(int bit0, int bit1) {
    uint32_t w[2] = {(uint32_t)bit0, (uint32_t)bit1};
    gq_push(GQ_MASK, w, 2);
}

void Draw_ClearImage(PS1_RECT *rect, u_char r, u_char g, u_char b) {
    if (!rect->w || !rect->h) return;
    uint32_t w[5];
    memcpy(w, rect, sizeof(PS1_RECT)); /* 8 bytes: x y w h */
    w[2] = r;
    w[3] = g;
    w[4] = b;
    gq_push(GQ_CLEAR, w, 5);
}

void Draw_MoveImage(PS1_RECT *rect, unsigned int x, unsigned int y) {
    if (!rect->w || !rect->h) return;
    uint32_t w[4];
    memcpy(w, rect, sizeof(PS1_RECT));
    w[2] = x;
    w[3] = y;
    gq_push(GQ_MOVE, w, 4);
}

/* Data in/out of VRAM: drain the queue, then run on this core. */
void Draw_LoadImage(PS1_RECT *rect, u_long *p) {
    if (!rect->w || !rect->h) return;
    uint32_t words = ((uint32_t)rect->w * (uint32_t)rect->h + 1) / 2;
    if (p && rect->w > 0 && rect->h > 0 && words <= GQ_LOAD_MAX_WORDS) {
        uint32_t w[2];
        memcpy(w, rect, sizeof(PS1_RECT));
        gq_push2(GQ_LOAD, w, 2, (const uint32_t *)p, words);
        return;
    }
    gpu_fence_c(2);
    before_write(rect);
    TIMED_RECT(1, rect, load_image_fast(rect, (const u16 *)p));
}
void Draw_StoreImage(PS1_RECT *rect, u_long *p) {
    if (!rect->w || !rect->h) return;
    gpu_fence_c(3);
    before_read(rect);
    TIMED_RECT(3, rect, soft_store_image(rect, (u16 *)p));
}

/* Called by video.c before waiting for vblank. A frame held in a half slot
 * is scanned out from SRAM; a whole-area one must be in PSRAM. */
void Gpu_FlushForDisplay(void) {
    gpu_fence_c(1); /* the frame about to be shown must be fully drawn */
    Slot *s = &slot[0];
    if (!s->whole || !s->used || !s->dirty || !s->loaded) return;
    fl_next = 0;
    fl_core1 = false;
    fl_slot = 0;
}

/* Core 0, in VSync while waiting for vblank: flush one more row. Returns
 * false when there is nothing (left) to flush. */
bool Gpu_FlushStep(void) {
    if (fl_slot < 0 || fl_core1) return false;
    Slot *s = &slot[fl_slot];
    uint64_t t = time_us_64();
    flush_rows(s, fl_next, fl_next + 1);
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
    if (++fl_next >= s->h) {
        s->dirty = false;
        fl_slot = -1;
    }
    return true;
}

/* Core 0, right after the vblank: finish the flush here. Core 1 is idle
 * (no commands for the next frame yet) and keeps the scanout fed, so core 0
 * copies at full speed; the rows left are bottom rows the beam reaches only
 * ~10 ms later. Handing them to core 1 instead cost twice as long per row
 * (it interleaves the scanout) and pushed its next frame past vblank. */
void Gpu_FlushHandOff(void) {
    if (fl_slot < 0) return;
    Slot *s = &slot[fl_slot];
    uint64_t t = time_us_64();
    frame_c1flush_rows += s->h - fl_next;
    flush_rows(s, fl_next, s->h);
    s->dirty = false;
    fl_slot = -1;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
    frame_c1flush_us += (uint32_t)(time_us_64() - t);
}

/* Core 1, before anything else: finish a handed-over display flush. */
void __not_in_flash_func(Gpu_Core1FlushPending)(void) {
    if (!fl_core1) return;
    Slot *s = &slot[fl_slot];
    uint64_t t = time_us_64();
    frame_c1flush_rows += s->h - fl_next;
    flush_rows(s, fl_next, s->h);
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
    frame_c1flush_us += (uint32_t)(time_us_64() - t);
    fl_slot = -1;
    __dmb();
    fl_core1 = false;
}

/* For the scanout (core 1, at latch time): if a loaded half slot holds the
 * display rectangle starting at (x,y), its rows are complete 320-pixel lines
 * (*row0 = start of the line for display row 0). The draw area is usually
 * shorter than the display (SOTN letterboxes rows 20..226), so only display
 * rows [*r0,*r1) come from it. */
bool __not_in_flash_func(Gpu_ScanoutSource)(int x, int y, const u16 **row0, int *stride, int *r0,
                                            int *r1) {
    for (int i = 0; i < 2; i++) {
        const Slot *s = &slot[i];
        if (!s->used || s->whole || !s->loaded || !s->margin || s->x0 != x ||
            s->y0 + s->h <= y || s->y0 >= y + 240)
            continue;
        *row0 = s->base - s->margin - s->pad + (y - s->y0) * s->sw;
        *stride = s->sw;
        *r0 = s->y0 > y ? s->y0 - y : 0;
        *r1 = s->y0 + s->h - y;
        return true;
    }
    return false;
}

void Gpu_DebugArea(int *a) {
    const SoftState *s = &soft_st[1];
    a[0] = s->x0; a[1] = s->y0; a[2] = s->x1; a[3] = s->y1; a[4] = s->ox; a[5] = s->oy;
}

void Draw_ResetBuffer(void) {}
void Draw_FlushBuffer(void) {}
/* DrawSync(): everything queued has been drawn */
/* Who calls DrawSync, and how long each call site waits (perf log): a
 * DrawSync early in the frame stalls the game until core 1 has finished the
 * previous frame, which removes the logic/raster overlap. */
#define DS_SITES 8
static volatile bool in_drawsync;
volatile uint32_t ds_site_pc[DS_SITES], ds_site_n[DS_SITES], ds_site_us[DS_SITES];
extern int __real_DrawSync(int mode);
int __wrap_DrawSync(int mode) {
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t t = time_us_32();
    in_drawsync = true;
    int r = __real_DrawSync(mode);
    in_drawsync = false;
    uint32_t dt = time_us_32() - t;
    for (int i = 0; i < DS_SITES; i++) {
        if (ds_site_pc[i] == pc || ds_site_pc[i] == 0) {
            ds_site_pc[i] = pc;
            ds_site_n[i]++;
            ds_site_us[i] += dt;
            break;
        }
    }
    return r;
}

int Draw_ExequeSync(void) {
    /* psyz also calls this from LoadImage/ClearImage/MoveImage to flush its
     * own packet queue. Our ring keeps every command in order and copies its
     * data, and StoreImage fences by itself, so only a real DrawSync has to
     * wait: fencing on every texture/palette upload made the game wait for
     * core 1 to finish the previous frame mid-logic. */
    if (!in_drawsync) return 0;
    gpu_sync_at_us += time_us_32() - psxv_vsync_ret_us;
    gpu_tl_frames++;
    gq_first_sum += gq_first_at;
    gq_last_sum += gq_last_at;
    gq_first_at = 0;
    /* useful work while core 1 finishes the frame: mix the audio now
     * instead of after VSync */
    extern bool Audio_IdleFill(void);
    extern void Psyz_CdPrefetch(void);
    /* Music sectors are read from the SD card here, early in the frame and
     * while core 1 is still drawing, so mixing (here or in VSync) never
     * waits on the card. Late in the frame neither starts: a read or a mix
     * step must not push the frame past vblank. The audio ring holds ~80 ms
     * and VSync tops it up if it ever runs low. */
    /* ...unless the read-ahead still holds two sectors (~100 ms of music):
     * then core 0 helps first and reads after, if the frame is still young */
    extern int Psyz_CdPrefetchLevel(void);
    bool prefetch_later = Psyz_CdPrefetchLevel() >= 2;
    if (!prefetch_later && time_us_32() - psxv_vsync_ret_us < 9000) Psyz_CdPrefetch();
    /* the audio this frame needs first, while core 1 draws on its own */
    extern bool Audio_PreHelpFill(void);
    while (Audio_PreHelpFill()) {
    }
    gpu_help();
    if (prefetch_later && time_us_32() - psxv_vsync_ret_us < 12000) Psyz_CdPrefetch();
    /* light frames (core 1 already done) still top the audio up here: the
     * frame is young, and VSync's emergency top-up is the expensive place */
    while (time_us_32() - psxv_vsync_ret_us < 9000 && Audio_IdleFill()) {
    }
    gpu_fence_c(0);
    gpu_drain_at_us += drain_last;
    gpu_last_sync_at = time_us_32() - psxv_vsync_ret_us;
    gpu_last_drain_at = drain_last;
    return 0;
}
