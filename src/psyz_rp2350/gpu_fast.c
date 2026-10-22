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

#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"

typedef long long s64;

u16 g_PsxVram[VRAM_W * VRAM_H] __attribute__((section(".psram_bss.vram"), aligned(64)));

/* ---- framebuffer slots ---- */
/* A half slot stores each row as a complete 320-pixel HDMI line: the draw
 * area centred between black margins, so the scanout can DMA SRAM rows
 * as-is. Sized for SOTN's 256x207 letterboxed play area (320x208); together
 * the two halves hold a 512x240 buffer. */
#define LINE_PX 320
#define SLOT_PIXELS (LINE_PX * 208)
#define SHADOW_PIXELS (2 * SLOT_PIXELS)
static u16 shadow_buf[SHADOW_PIXELS] __attribute__((aligned(4)));

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

static Slot slot[2];
static int cur = -1;          /* slot DROW() draws into, -1 = PSRAM */
static u16 *draw_origin;      /* draw_origin + y*sw + x == pixel (x,y) */
static int draw_sw;
volatile uint32_t gpu_shadow_us;

#define DROW(y) (cur >= 0 ? draw_origin + (y) * draw_sw : g_PsxVram + (y) * VRAM_W)

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

static void slot_flush(Slot *s) {
    if (!s->used || !s->dirty || !s->loaded) return;
    uint64_t t = time_us_64();
    for (int j = 0; j < s->h; j++) {
        u16 *row = g_PsxVram + (s->y0 + j) * VRAM_W + s->x0;
        row_cache_sync(row, s->w, true);
        copy_px((u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), s->base + j * s->sw, s->w);
    }
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
}

static void slot_load(Slot *s) {
    uint64_t t = time_us_64();
    for (int j = 0; j < s->h; j++) {
        const u16 *row = g_PsxVram + (s->y0 + j) * VRAM_W + s->x0;
        row_cache_sync(row, s->w, false);
        copy_px(s->base + j * s->sw, (const u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), s->w);
    }
    s->loaded = true;
    s->dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
}

static void slot_release(Slot *s) {
    slot_flush(s);
    s->used = false;
}

static void flush_overlapping(int x, int y, int w, int h) {
    for (int i = 0; i < 2; i++)
        if (overlaps(&slot[i], x, y, w, h)) slot_flush(&slot[i]);
}

/* the rectangle the scanout will show next (see video.c) */
extern bool Psyz_DisplayOrigin(int *x, int *y);
static bool is_display_rect(const Slot *s) {
    int dx = -1, dy = -1;
    return Psyz_DisplayOrigin(&dx, &dy) && dx == s->x0 && s->y0 >= dy && s->y0 < dy + 240;
}

/* Find or allocate the slot for rect; -1 if it does not fit. */
static int slot_for(int x, int y, int w, int h) {
    for (int i = 0; i < 2; i++)
        if (slot[i].used && slot[i].x0 == x && slot[i].y0 == y && slot[i].w == w && slot[i].h == h)
            return i;
    int pad = x & 1, sw = (w + pad + 1) & ~1, margin = 0;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H ||
        sw * h > SHADOW_PIXELS)
        return -1;
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
    for (int i = 0; i < 2; i++)
        if (slot[i].dirty && (overlaps(&slot[i], px, py, pw, 256) ||
                              overlaps(&slot[i], (clut & 0x3F) * 16, (clut >> 6) & 0x1FF, 256, 1)))
            slot_flush(&slot[i]);
}
#define TEX_HOOK(tpage, clut) tex_hook(tpage, clut)

#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#define SOFT_CLUT_CACHE
#define SOFT_RASTER_IRAM __attribute__((section(".time_critical.raster"), noinline))
#include "soft_raster.inc.c"

/* profiling: microseconds spent rasterizing / moving VRAM, read by video.c */
volatile uint32_t gpu_prim_us, gpu_vram_us, gpu_prims;

void Draw_Reset(void) {
    slot[0].used = slot[1].used = false;
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
    uint64_t t = time_us_64();
    int i = slot_for(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1);
    if (i >= 0) {
        if (!slot[i].loaded) slot_load(&slot[i]);
        slot[i].dirty = true;
    }
    draw_into(i);
    int r = soft_push_prim(packets, max_len);
    gpu_prim_us += (uint32_t)(time_us_64() - t);
    gpu_prims++;
    return r;
}

#define TIMED(call)                                                   \
    do {                                                              \
        uint64_t t0_ = time_us_64();                                  \
        call;                                                         \
        gpu_vram_us += (uint32_t)(time_us_64() - t0_);                \
    } while (0)

/* A VRAM op reads rect in PSRAM: publish overlapping slots first. */
static void before_read(const PS1_RECT *r) { flush_overlapping(r->x, r->y, r->w, r->h); }

/* A VRAM op writes rect in PSRAM: overlapping slots must reload it. */
static void before_write(const PS1_RECT *r) {
    for (int i = 0; i < 2; i++)
        if (overlaps(&slot[i], r->x, r->y, r->w, r->h)) {
            slot_flush(&slot[i]);
            slot[i].loaded = false;
        }
}

void Draw_ClearImage(PS1_RECT *rect, u_char r, u_char g, u_char b) {
    if (!rect->w || !rect->h) return;
    uint64_t t0 = time_us_64();
    /* clearing a whole framebuffer (a slot's rectangle, or the draw area):
     * fill its slot, no load needed */
    int i = -1;
    for (int k = 0; k < 2; k++)
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
    }
    gpu_vram_us += (uint32_t)(time_us_64() - t0);
}
void Draw_LoadImage(PS1_RECT *rect, u_long *p) {
    if (!rect->w || !rect->h) return;
    before_write(rect);
    TIMED(soft_load_image(rect, (const u16 *)p));
}
void Draw_StoreImage(PS1_RECT *rect, u_long *p) {
    if (!rect->w || !rect->h) return;
    before_read(rect);
    TIMED(soft_store_image(rect, (u16 *)p));
}
void Draw_MoveImage(PS1_RECT *rect, unsigned int x, unsigned int y) {
    if (!rect->w || !rect->h) return;
    PS1_RECT dst = {(short)x, (short)y, rect->w, rect->h};
    before_read(rect);
    before_write(&dst);
    TIMED(soft_move_image(rect, (int)x, (int)y));
}

/* Called by video.c before waiting for vblank. A frame held in a half slot
 * is scanned out from SRAM; a whole-area one must be in PSRAM. */
void Gpu_FlushForDisplay(void) {
    if (slot[0].whole) slot_flush(&slot[0]);
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
int Draw_ExequeSync(void) { return 0; }
