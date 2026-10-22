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
static int cur = -1;          /* slot DROW() draws into, -1 = PSRAM */
static u16 *draw_origin;      /* draw_origin + y*sw + x == pixel (x,y) */
static int draw_sw;
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
#define DROW(y) (scan_poll(), cur >= 0 ? draw_origin + (y) * draw_sw : g_PsxVram + (y) * VRAM_PITCH)

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

/* What each VRAM row in PSRAM last received from a slot flush: x range and
 * a hash of the pixels. Title and menu screens redraw the same backdrop every
 * frame, and with double buffering a PSRAM buffer was written two frames
 * ago with (mostly) the same rows: those are skipped instead of copied.
 * Anything else writing PSRAM VRAM forgets the rows it touches
 * (vrow_forget). */
static uint32_t vrow_hash[VRAM_H];
static uint16_t vrow_x0[VRAM_H], vrow_w[VRAM_H]; /* w == 0: unknown */

static void vrow_forget(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (h > VRAM_H) h = VRAM_H;
    for (int j = 0; j < h; j++) {
        int r = (y + j) & (VRAM_H - 1);
        if (vrow_w[r] && x < vrow_x0[r] + vrow_w[r] && x + w > vrow_x0[r]) vrow_w[r] = 0;
    }
}

static inline uint32_t row_hash(const u16 *p, int n) {
    const uint32_t *q = (const uint32_t *)p;
    uint32_t h = 2166136261u ^ (uint32_t)n;
    int k = 0;
    for (; k + 1 < n; k += 2) h = (h ^ *q++) * 16777619u;
    if (n & 1) h = (h ^ p[n - 1]) * 16777619u;
    return h;
}

/* Slot rows go to PSRAM by CPU stores. Not by DMA: a DMA channel writing
 * PSRAM stalls the DMA master on every write and the HSTX channels with it
 * (tried: 2.5x faster copy, no HDMI signal). */
static void flush_rows(Slot *s, int jfrom, int jto) {
    bool hashable = !(((uintptr_t)s->base) & 3) && !(s->sw & 1);
    for (int j = jfrom; j < jto; j++) {
        int vy = (s->y0 + j) & (VRAM_H - 1);
        u16 *row = g_PsxVram + vy * VRAM_PITCH + s->x0;
        const u16 *src = s->base + j * s->sw;
        if (get_core_num() == 1) Video_ScanSync();
        if (hashable) {
            uint32_t h = row_hash(src, s->w);
            if (vrow_w[vy] == s->w && vrow_x0[vy] == s->x0 && vrow_hash[vy] == h) {
                flush_rows_skipped++; /* PSRAM already holds exactly this row */
                continue;
            }
            vrow_hash[vy] = h;
            vrow_x0[vy] = (uint16_t)s->x0;
            vrow_w[vy] = (uint16_t)s->w;
        } else {
            vrow_forget(s->x0, vy, s->w, 1);
        }
        uint32_t c0 = time_us_32();
        row_cache_sync(row, s->w, true);
        uint32_t c1 = time_us_32();
        copy_px((u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), src, s->w);
        flush_maint_us += c1 - c0;
        flush_copy_us += time_us_32() - c1;
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

static void slot_flush_c(Slot *s, int cause) {
    if (!s->used || !s->dirty || !s->loaded) return;
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
    int pad = x & 1, sw = (w + pad + 1) & ~1, margin = 0;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H ||
        sw * h > SHADOW_PIXELS)
        return -1;
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

static void draw_into(int i) {
    cur = i;
    if (i < 0) return;
    Slot *s = &slot[i];
    draw_sw = s->sw;
    draw_origin = s->base - s->y0 * s->sw - s->x0;
}

/* Before sampling texels/CLUT that live in a slot, publish it to PSRAM. */
static void tex_hook(u16 tpage, u16 clut) {
    int bpp = (tpage >> 7) & 3;
    int px = (tpage & 0xF) * 64, py = ((tpage >> 4) & 1) * 256;
    int pw = bpp == 0 ? 64 : bpp == 1 ? 128 : 256;
    for (int i = 0; i < NSLOT; i++)
        if (slot[i].dirty && (overlaps(&slot[i], px, py, pw, 256) ||
                              overlaps(&slot[i], (clut & 0x3F) * 16, (clut >> 6) & 0x1FF, 256, 1)))
            slot_flush_c(&slot[i], CAUSE_TEX);
}
#define TEX_HOOK(tpage, clut) tex_hook(tpage, clut)

/* 8/15bpp sprite rows through the XIP stream engine (video.c) */
extern bool Video_TexFetch(const void *src, void *dst, uint32_t words);
extern void Video_TexWait(void);
#define SOFT_ROW_FETCH(src, dst, words) Video_TexFetch(src, dst, words)
#define SOFT_ROW_WAIT() Video_TexWait()






#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#define SOFT_CLUT_CACHE
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

/* hdr words, then payload words */
static void gq_push2(uint32_t type, const uint32_t *w, uint32_t n, const uint32_t *p, uint32_t pn) {
    uint32_t total = n + pn;
    while (GQ_WORDS - (gq_head - gq_tail) < total + 1) tight_loop_contents(); /* ring full */
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

/* ---- core 1: execution ---- */

/* poly flat, poly tex, poly tex+gouraud, line, sprite, tile; +6 semi */
volatile uint32_t gpu_class_us[12], gpu_class_n[12], gpu_class_px[12];
static void exec_prim(u_long *packets, int len) {
    uint64_t t = time_us_64();
    gpu_c1_state = 1;
    int i = slot_for(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1);
    if (i >= 0) {
        if (!slot[i].loaded) slot_load(&slot[i]);
        slot[i].dirty = true;
    }
    draw_into(i);
    soft_push_prim(packets, len);
    if (i < 0) vrow_forget(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1); /* drew into PSRAM */
    uint32_t dt = (uint32_t)(time_us_64() - t);
    gpu_prim_us += dt;
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
    TIMED(soft_move_image(rect, x, y));
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
        uint32_t t = gq_tail;
        uint32_t hdr = gq[t & (GQ_WORDS - 1)];
        uint32_t type = hdr >> 24, n = hdr & 0xFFFFFF;
        if (type == GQ_LOAD) {
            for (uint32_t i = 0; i < n; i++) load_buf[i] = gq[(t + 1 + i) & (GQ_WORDS - 1)];
        } else {
            for (uint32_t i = 0; i < n && i < 64; i++) w[i] = gq[(t + 1 + i) & (GQ_WORDS - 1)];
        }
        switch (type) {
        case GQ_PRIM: exec_prim((u_long *)w, (int)n); break;
        case GQ_TPAGE: soft_set_texpage_mode((ParamDrawTexpageMode *)&w[0]); break;
        case GQ_TWIN: soft_set_texture_window(w[0], w[1], w[2], w[3]); break;
        case GQ_AREA0: soft_set_area_start((int)w[0], (int)w[1]); break;
        case GQ_AREA1: soft_set_area_end((int)w[0], (int)w[1]); break;
        case GQ_OFFSET: soft_set_offset((int)w[0], (int)w[1]); break;
        case GQ_MASK: soft_set_mask((int)w[0], (int)w[1]); break;
        case GQ_CLEAR: exec_clear((PS1_RECT *)&w[0], (u_char)w[2], (u_char)w[3], (u_char)w[4]); break;
        case GQ_MOVE: exec_move((PS1_RECT *)&w[0], (int)w[2], (int)w[3]); break;
        case GQ_LOAD: {
            PS1_RECT *r = (PS1_RECT *)&load_buf[0];
            before_write(r);
            TIMED(load_image_fast(r, (const u16 *)&load_buf[2]));
            break;
        }
        }
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
    cur = -1;
    clut_cache_flush();
    st = (SoftState){0, 0, VRAM_W - 1, VRAM_H - 1, 0, 0};
    cur_tpage = 0;
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
    TIMED(load_image_fast(rect, (const u16 *)p));
}
void Draw_StoreImage(PS1_RECT *rect, u_long *p) {
    if (!rect->w || !rect->h) return;
    gpu_fence_c(3);
    before_read(rect);
    TIMED(soft_store_image(rect, (u16 *)p));
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

/* Core 0, after the vblank: core 1 finishes what is left. */
void Gpu_FlushHandOff(void) {
    if (fl_slot < 0) return;
    __dmb();
    fl_core1 = true;
}

/* Core 1, before anything else: finish a handed-over display flush. */
void __not_in_flash_func(Gpu_Core1FlushPending)(void) {
    if (!fl_core1) return;
    Slot *s = &slot[fl_slot];
    uint64_t t = time_us_64();
    flush_rows(s, fl_next, s->h);
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
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

void Gpu_DebugArea(int *a) { a[0] = st.x0; a[1] = st.y0; a[2] = st.x1; a[3] = st.y1; a[4] = st.ox; a[5] = st.oy; }

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
    /* useful work while core 1 finishes the frame: mix the audio now
     * instead of after VSync */
    extern bool Audio_IdleFill(void);
    extern void Psyz_CdPrefetch(void);
    /* Music sectors are read from the SD card here, early in the frame and
     * while core 1 is still drawing, so mixing (here or in VSync) never
     * waits on the card. Late in the frame neither starts: a read or a mix
     * step must not push the frame past vblank. The audio ring holds ~80 ms
     * and VSync tops it up if it ever runs low. */
    if (time_us_32() - psxv_vsync_ret_us < 9000) Psyz_CdPrefetch();
    while ((gq_head != gq_tail || gq_exec) && time_us_32() - psxv_vsync_ret_us < 9000 &&
           Audio_IdleFill()) {
    }
    gpu_fence_c(0);
    gpu_drain_at_us += drain_last;
    gpu_last_sync_at = time_us_32() - psxv_vsync_ret_us;
    gpu_last_drain_at = drain_last;
    return 0;
}
