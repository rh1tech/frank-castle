/*
 * frank-castle — PS1 GPU for RP2350: shell around soft_raster.inc.c.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later (this shell; the included
 * rasterizer core is MPL-2.0)
 *
 * VRAM is a 1024x512 BGR555 array in PSRAM shared with the HDMI scanout
 * (video.c). The rasterizer loops run from SRAM; CLUTs are cached in SRAM.
 *
 * Draw shadow: PSRAM costs ~200 ns per scattered 16-bit access (8-byte XIP
 * cache lines, thrashed by code fetches), so the current draw area is
 * rendered into an SRAM copy instead. It binds to the draw area on the first
 * primitive, or on a ClearImage of the whole area (no load needed then), and
 * is flushed to PSRAM at VSync so the scanout sees the finished frame. VRAM
 * block ops and texture/CLUT fetches that overlap it keep both copies
 * coherent.
 */

#include <psyz.h>
#include <libgpu.h>
#include <stdbool.h>
#include <string.h>
#include "internal.h"
#include "draw.h"

#include "pico/time.h"
#include "hardware/xip_cache.h"
#include "hardware/regs/addressmap.h"

typedef long long s64;

u16 g_PsxVram[VRAM_W * VRAM_H] __attribute__((section(".psram_bss.vram"), aligned(64)));

/* ---- SRAM draw shadow ---- */
#define SHADOW_PIXELS (128 * 1024) /* 256 KB: 512x256 or 256x512 */
static u16 shadow_buf[SHADOW_PIXELS] __attribute__((aligned(4)));
static struct {
    int x0, y0, w, h;
    bool bound;  /* DROW() points into shadow_buf */
    bool loaded; /* shadow holds the area's current content */
    bool dirty;  /* shadow is newer than PSRAM */
} sh;
static u16 *shadow_origin; /* shadow_origin + y*w + x == pixel (x,y) */
volatile uint32_t gpu_shadow_us;

#define DROW(y) (sh.bound ? shadow_origin + (y) * sh.w : g_PsxVram + (y) * VRAM_W)

static bool rect_overlaps(int x, int y, int w, int h) {
    return sh.bound && x < sh.x0 + sh.w && x + w > sh.x0 && y < sh.y0 + sh.h && y + h > sh.y0;
}

/* Bulk copies between the shadow and PSRAM go through the uncached alias
 * (0x15xxxxxx): through the cache every 8-byte line costs a write-allocate
 * read plus a write-back. The cached copy of each row is cleaned (pending
 * writes committed) and, before a write, invalidated so no stale line can
 * be read or evicted over the new data. */
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

static void shadow_flush(void) {
    if (!sh.bound || !sh.dirty || !sh.loaded) return;
    uint64_t t = time_us_64();
    for (int j = 0; j < sh.h; j++) {
        u16 *row = g_PsxVram + (sh.y0 + j) * VRAM_W + sh.x0;
        row_cache_sync(row, sh.w, true);
        copy_px((u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), shadow_buf + j * sh.w, sh.w);
    }
    sh.dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
}

static void shadow_load(void) {
    uint64_t t = time_us_64();
    for (int j = 0; j < sh.h; j++) {
        const u16 *row = g_PsxVram + (sh.y0 + j) * VRAM_W + sh.x0;
        row_cache_sync(row, sh.w, false);
        copy_px(shadow_buf + j * sh.w, (const u16 *)((uintptr_t)row + PSRAM_UNCACHED_OFFSET), sh.w);
    }
    sh.loaded = true;
    sh.dirty = false;
    gpu_shadow_us += (uint32_t)(time_us_64() - t);
}

static void shadow_unbind(void) {
    shadow_flush();
    sh.bound = false;
}

/* Point the shadow at rect (x,y,w,h); false if it does not fit. */
static bool shadow_bind_rect(int x, int y, int w, int h) {
    if (sh.bound && sh.x0 == x && sh.y0 == y && sh.w == w && sh.h == h) return true;
    shadow_unbind();
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H ||
        w * h > SHADOW_PIXELS)
        return false;
    sh.x0 = x;
    sh.y0 = y;
    sh.w = w;
    sh.h = h;
    sh.bound = true;
    sh.loaded = false;
    sh.dirty = false;
    shadow_origin = shadow_buf - y * w - x;
    return true;
}

/* Before sampling texels/CLUT that live in the shadowed area, publish it. */
static void tex_hook(u16 tpage, u16 clut) {
    if (!sh.bound || !sh.dirty) return;
    int bpp = (tpage >> 7) & 3;
    int px = (tpage & 0xF) * 64, py = ((tpage >> 4) & 1) * 256;
    int pw = bpp == 0 ? 64 : bpp == 1 ? 128 : 256;
    if (rect_overlaps(px, py, pw, 256) ||
        rect_overlaps((clut & 0x3F) * 16, (clut >> 6) & 0x1FF, 256, 1))
        shadow_flush();
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
    sh.bound = false;
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
    if (shadow_bind_rect(st.x0, st.y0, st.x1 - st.x0 + 1, st.y1 - st.y0 + 1)) {
        if (!sh.loaded) shadow_load();
        sh.dirty = true;
    }
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

/* Make PSRAM authoritative for rect before a VRAM op reads it. */
static void before_read(const PS1_RECT *r) {
    if (rect_overlaps(r->x, r->y, r->w, r->h)) shadow_flush();
}
/* After a VRAM op wrote rect in PSRAM, the shadow must reload it. */
static void before_write(const PS1_RECT *r) {
    if (rect_overlaps(r->x, r->y, r->w, r->h)) {
        shadow_flush();
        sh.loaded = false;
    }
}

void Draw_ClearImage(PS1_RECT *rect, u_char r, u_char g, u_char b) {
    if (!rect->w || !rect->h) return;
    uint64_t t0 = time_us_64();
    bool is_draw_area = rect->x == st.x0 && rect->y == st.y0 &&
                        rect->w == st.x1 - st.x0 + 1 && rect->h == st.y1 - st.y0 + 1;
    bool covers = sh.bound && rect->x <= sh.x0 && rect->y <= sh.y0 &&
                  rect->x + rect->w >= sh.x0 + sh.w && rect->y + rect->h >= sh.y0 + sh.h;
    if ((is_draw_area && shadow_bind_rect(rect->x, rect->y, rect->w, rect->h)) || covers) {
        /* fill the shadow; PSRAM only for any part of rect outside it */
        u16 c = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
        uint32_t c2 = c | ((uint32_t)c << 16), *p = (uint32_t *)shadow_buf;
        for (int i = 0; i < sh.w * sh.h / 2; i++) p[i] = c2;
        if (sh.w * sh.h & 1) shadow_buf[sh.w * sh.h - 1] = c;
        sh.loaded = true;
        sh.dirty = true;
        clut_cache_flush();
        if (rect->w != sh.w || rect->h != sh.h) {
            bool b2 = sh.bound;
            sh.bound = false; /* keep the soft_ op off the shadow */
            soft_clear_image(rect, r, g, b);
            sh.bound = b2;
        }
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

/* Called by video.c before waiting for vblank: the frame must be in PSRAM. */
void Gpu_FlushForDisplay(void) { shadow_flush(); }

void Draw_ResetBuffer(void) {}
void Draw_FlushBuffer(void) {}
int Draw_ExequeSync(void) { return 0; }
