// frank-castle: taken from the ESP32-S3 SOTN port's psyz fork
// (github.com/davidmonterocrespo24/psyz @97b064c, psyz/src/platform/
// soft_raster.inc.c, MPL-2.0); this file stays under the MPL-2.0.
// Changes: display state removed (video.c owns it), entry points renamed
// soft_* so gpu_fast.c can wrap them.
// PSY-Z software rasterizer - portable core.
//
// Plain C over a u16[512][1024] VRAM in native PSX RGB5551. No SDL, no OS, no
// allocations: this translation unit is compiled unchanged into both the PC
// harness (soft_gpu.c includes it) and the ESP32-S3 firmware. Anything
// platform-shaped belongs in the including shell, not here.
//
// It is written as an includable .inc.c on purpose: the shell provides `vram`
// resolution and the packet parser, the core provides state + rasterization +
// VRAM block ops, and both compile as ONE unit so the hot paths inline.

// ---- shared-macro shims -----------------------------------------------------
// When the PC shell includes sdl3_common.h first it already provides these;
// the ESP32 build defines SOFT_RASTER_STANDALONE and gets them here.
#ifdef SOFT_RASTER_STANDALONE
#include <stdbool.h>
// PS1_RECT is sdl3_common's rename of libgpu's RECT (dodging windef's on
// Windows); standalone builds have no windef to dodge.
#define PS1_RECT RECT
#define SEMITRANSP 0x02
#define TEXTURED 0x04
#define EXTRA_VERTEX 0x08
#define GOURAUD 0x10
#define TRIANGLE 0x20
static unsigned short cur_tpage = 0;
static short s11(short v) { return (short)(((v & 0x7FF) ^ 1024) - 1024); }
#ifndef WARNF
#define WARNF(...)
#endif
static void soft_set_texpage_mode(ParamDrawTexpageMode* p) {
    cur_tpage = (*(unsigned short*)p) & 0x1FF;
}
static void soft_set_texture_window(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {
    (void)mask_x; (void)mask_y; (void)off_x; (void)off_y;
}
static void soft_set_mask(int bit0, int bit1) { (void)bit0; (void)bit1; }
#endif // SOFT_RASTER_STANDALONE

// ==================== core (SDL-free, ESP32-ready) ====================

// The game's VRAM when linked into SOTN; local fallback otherwise. Declared
// weak so psyz still links standalone.
#ifdef SOFT_RASTER_NO_FALLBACK
// The embedding shell owns VRAM placement outright (the ESP32 build puts it in
// PSRAM with an explicit attribute); a megabyte of fallback .bss would not
// even link against 512KB of internal RAM.
#elif defined(_MSC_VER)
static u16 soft_vram_fallback[VRAM_H * VRAM_W];
#define SOFT_VRAM soft_vram_fallback
#else
extern u16 g_RawVram[VRAM_H * VRAM_W] __attribute__((weak));
static u16 soft_vram_fallback[VRAM_H * VRAM_W];
#define SOFT_VRAM (g_RawVram ? g_RawVram : soft_vram_fallback)
#endif

static u16* const vram = g_PsxVram; // frank-castle: PSRAM VRAM (gpu_fast.c)

// frank-castle: pixel writes go through DROW(y), a pointer such that
// DROW(y)[x] is VRAM pixel (x,y) -- in the SRAM draw shadow when one is bound
// (gpu_fast.c), else in PSRAM. TEX_HOOK runs before textures are sampled.
#ifndef DROW
#define DROW(y) (vram + (y) * VRAM_PITCH)
#endif
#ifndef TEX_HOOK
#define TEX_HOOK(tpage, clut)
#endif
// frank-castle: every texel read goes through TEX_RD(p) (gpu_fast.c puts a
// software texture cache in SRAM behind it)
#ifndef TEX_RD
#define TEX_RD(p) (*(p))
#endif

typedef struct {
    int x0, y0, x1, y1; // draw area, inclusive
    int ox, oy;         // draw offset
} SoftState;
static SoftState st = {0, 0, VRAM_W - 1, VRAM_H - 1, 0, 0};

// One vertex after packet parsing. Colors stay 8-bit until the pixel write.
typedef struct {
    int x, y;
    int u, v;
    int r, g, b;
} SV;

// The embedded build attributes the rasterizer loops into IRAM and swaps the
// PSRAM CLUT pointer for a small internal-SRAM cache; on PC both default off.
#ifndef SOFT_RASTER_IRAM
#define SOFT_RASTER_IRAM
#endif

#ifdef SOFT_CLUT_CACHE
// 8-way store of recently used CLUTs (4KB DRAM). Texel fetches then cost one
// PSRAM read instead of two. Any VRAM image op invalidates everything - CLUT
// uploads and palette animation go through LoadImage.
#ifndef SOFT_CLUT_WAYS
#define SOFT_CLUT_WAYS 8
#endif
static u16 clut_cache[SOFT_CLUT_WAYS][256];
static u16 clut_cache_key[SOFT_CLUT_WAYS];
static u8 clut_cache_next;
static void clut_cache_flush(void) {
    for (int i = 0; i < SOFT_CLUT_WAYS; i++) {
        clut_cache_key[i] = 0xFFFF;
    }
}
static inline const u16* clut_cached(u16 id, const u16* src, int n) {
    for (int i = 0; i < SOFT_CLUT_WAYS; i++) {
        if (clut_cache_key[i] == id) {
            return clut_cache[i];
        }
    }
    int slot = clut_cache_next++ % SOFT_CLUT_WAYS;
    memcpy(clut_cache[slot], src, n * sizeof(u16));
    clut_cache_key[slot] = id;
    return clut_cache[slot];
}
#else
#define clut_cache_flush()
#endif

// ---- pixel write -----------------------------------------------------------

// Semi-transparency modes, tpage bits 5-6. B = background (dest), F = front.
//   0: B/2 + F/2    1: B + F    2: B - F    3: B + F/4
static inline u16 blend5551(u16 back, int fr5, int fg5, int fb5, int abr) {
    int br = back & 0x1F, bg = (back >> 5) & 0x1F, bb = (back >> 10) & 0x1F;
    int r, g, b;
    switch (abr) {
    case 0:
        r = (br + fr5) >> 1;
        g = (bg + fg5) >> 1;
        b = (bb + fb5) >> 1;
        break;
    case 1:
        r = br + fr5;
        g = bg + fg5;
        b = bb + fb5;
        break;
    case 2:
        r = br - fr5;
        g = bg - fg5;
        b = bb - fb5;
        break;
    default:
        r = br + (fr5 >> 2);
        g = bg + (fg5 >> 2);
        b = bb + (fb5 >> 2);
        break;
    }
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (u16)(r | (g << 5) | (b << 10));
}

// Same four modes on packed RGB555 values (bit 15 of either input ignored,
// result has it clear): per-channel results identical to blend5551, without
// unpacking. Add/subtract spread the 5-bit fields into 10-bit lanes so they
// cannot carry or borrow into each other, then saturate per lane.
static inline uint32_t spread555(uint32_t c) {
    return (c & 0x1F) | ((c & 0x3E0) << 5) | ((c & 0x7C00) << 10);
}
static inline u16 pack555(uint32_t x) {
    return (u16)((x & 0x1F) | ((x >> 5) & 0x3E0) | ((x >> 10) & 0x7C00));
}
static inline u16 blend555(u16 back, u16 front, int abr) {
    uint32_t b = back & 0x7FFF, f = front & 0x7FFF;
    if (abr == 0) {
        return (u16)((b & f) + (((b ^ f) & 0x7BDE) >> 1)); // (B + F) >> 1
    }
    uint32_t xb = spread555(b), xf = spread555(f);
    if (abr == 2) { // B - F, clamped at 0
        uint32_t d = (xb | 0x02008020) - xf;        // lanes hold 32 + B - F
        uint32_t m = d & 0x02008020;                // lane >= 32: B >= F
        return pack555(d & (m - (m >> 5)));         // keep only those lanes
    }
    if (abr == 3) {
        xf = (xf >> 2) & 0x00701C07;                // F / 4 per lane
    }
    uint32_t sum = xb + xf;                         // B + F (or F/4), <= 62
    uint32_t m = sum & 0x02008020;                  // lanes that passed 31
    return pack555((sum | (m - (m >> 5))) & 0x01F07C1F);
}

// PS1 texture modulation: out5 = min(31, tex5 * col8 >> 7). 0x80 is neutral.
static inline int mod5(int tex5, int col8) {
    int v = (tex5 * col8) >> 7;
    return v > 31 ? 31 : v;
}

// ---- texture fetch ---------------------------------------------------------

typedef struct {
    const u16* page;  // top-left of the texture page in VRAM
    const u16* clut;  // 16 or 256 entries
    int bpp;          // 0=4bpp, 1=8bpp, 2=15bpp
    int abr;          // semi-transparency mode from the tpage
} Tex;

static unsigned soft_px; // pixels spanned (perf counters)
int soft_semi_log[6][5], soft_semi_logged; // big semi-transparent sprites (diagnostic)

static inline Tex tex_from(u16 tpage, u16 clut) {
    Tex t;
    TEX_HOOK(tpage, clut);
    int px = (tpage & 0xF) * 64;         // halfword units
    int py = ((tpage >> 4) & 1) * 256;
    t.page = vram + py * VRAM_PITCH + px;
    t.bpp = (tpage >> 7) & 3;
    t.abr = (tpage >> 5) & 3;
    t.clut = vram + ((clut >> 6) & 0x1FF) * VRAM_PITCH + (clut & 0x3F) * 16;
#ifdef SOFT_CLUT_CACHE
    if (t.bpp <= 1) {
        t.clut = (u16*)clut_cached(clut, t.clut, t.bpp == 0 ? 16 : 256);
    }
#endif
    return t;
}

static inline u16 tex_fetch(const Tex* t, int u, int v) {
    u &= 0xFF;
    v &= 0xFF;
    switch (t->bpp) {
    case 0: { // 4bpp: 4 texels per halfword
        u16 hw = TEX_RD(&t->page[v * VRAM_PITCH + (u >> 2)]);
        return t->clut[(hw >> ((u & 3) * 4)) & 0xF];
    }
    case 1: { // 8bpp: 2 texels per halfword
        u16 hw = TEX_RD(&t->page[v * VRAM_PITCH + (u >> 1)]);
        return t->clut[(hw >> ((u & 1) * 8)) & 0xFF];
    }
    default: // 15bpp direct
        return TEX_RD(&t->page[v * VRAM_PITCH + u]);
    }
}

// ---- primitive rasterizers -------------------------------------------------

// Shared per-pixel tail for textured pixels. Returns without writing on the
// PSX "texel 0000 is never drawn" rule; blends only texels with the STP bit
// when the primitive is semi-transparent.
#ifdef ESP_PLATFORM
unsigned psyz_texel_zero, psyz_texel_ok, psyz_flat_px;
unsigned psyz_tri_off, psyz_tri_tiny, psyz_tri_ok;
/* polygons the PSX GPU would have refused for exceeding 1023x511 */
unsigned psyz_tri_huge;
#endif
static inline void put_texel(u16* dst, u16 texel, int r8, int g8, int b8,
                             int semi, int abr) {
    if (texel == 0) {
#ifdef ESP_PLATFORM
        psyz_texel_zero++;
#endif
        return;
    }
#ifdef ESP_PLATFORM
    psyz_texel_ok++;
#endif
    // 0x80 modulation is neutral: mod5(t,128)==t, so the whole mul/clamp
    // chain collapses to a passthrough (bit-exact with the general form)
    if (r8 == 0x80 && g8 == 0x80 && b8 == 0x80) {
        if (semi && (texel & 0x8000)) {
            *dst = blend5551(*dst, texel & 0x1F, (texel >> 5) & 0x1F,
                             (texel >> 10) & 0x1F, abr);
        } else {
            *dst = texel;
        }
        return;
    }
    int r = mod5(texel & 0x1F, r8);
    int g = mod5((texel >> 5) & 0x1F, g8);
    int b = mod5((texel >> 10) & 0x1F, b8);
    if (semi && (texel & 0x8000)) {
        *dst = blend5551(*dst, r, g, b, abr);
    } else {
        *dst = (u16)(r | (g << 5) | (b << 10) | (texel & 0x8000));
    }
}

static inline void put_flat(u16* dst, int r8, int g8, int b8, int semi,
                            int abr) {
#ifdef ESP_PLATFORM
    psyz_flat_px++;
#endif
    int r = r8 >> 3, g = g8 >> 3, b = b8 >> 3;
    if (semi) {
        *dst = blend5551(*dst, r, g, b, abr);
    } else {
        *dst = (u16)(r | (g << 5) | (b << 10));
    }
}

// Span inner loops for the textured triangle paths, one function per texel
// depth: with the whole triangle setup inlined around it, the per-pixel loop
// ran out of registers and reloaded half its state from the stack on every
// pixel. Same stepping and arithmetic as the generic loop: identical output.
typedef struct {
    const u16* page;
    const u16* clut;
    int32_t udx, vdx, rdx, gdx, bdx;
    int semi, abr;
    const u16 *lr, *lg, *lb; // flat modulation tables (exactly mod5(c, col))
    int neutral;             // colour 0x808080: the tables are the identity
    const uint32_t* mclut;   // modulated palette (sprites): colour | STP | 1<<16 if drawn
} SpanCtx;

// 4bpp: one halfword holds four texels; re-read it only when the sample
// moves to another one (scaled and rotated spans revisit it often)
#define TEXEL4                                                                 \
    do {                                                                       \
        int key_ = v << 8 | u >> 2;                                            \
        if (key_ != hw_key) {                                                  \
            hw_key = key_;                                                     \
            hw = TEX_RD(&page[v * VRAM_PITCH + (u >> 2)]);                     \
        }                                                                      \
        texel = clut[(hw >> ((u & 3) * 4)) & 0xF];                             \
    } while (0)
#define TEXEL8 texel = clut[(TEX_RD(&page[v * VRAM_PITCH + (u >> 1)]) >> ((u & 1) * 8)) & 0xFF]
#define TEXEL15 texel = TEX_RD(&page[v * VRAM_PITCH + u])

#define DEF_SPAN_TF(NAME, FETCH)                                                \
    SOFT_RASTER_IRAM static void NAME(u16* dst, int n, int32_t ui, int32_t vi, \
                                      const SpanCtx* c) {                       \
        const u16 *page = c->page, *clut = c->clut;                             \
        const u16 *lr = c->lr, *lg = c->lg, *lb = c->lb;                        \
        int32_t udx = c->udx, vdx = c->vdx;                                     \
        int semi = c->semi;                                                     \
        int hw_key = -1;                                                        \
        u16 hw = 0;                                                             \
        (void)clut;                                                             \
        (void)hw_key;                                                           \
        (void)hw;                                                               \
        for (; n > 0; n--, dst++, ui += udx, vi += vdx) {                       \
            int u = (ui >> 16) & 0xFF, v = (vi >> 16) & 0xFF;                   \
            u16 texel;                                                          \
            FETCH;                                                              \
            if (!texel) continue;                                               \
            u16 m = c->neutral ? (u16)(texel & 0x7FFF)                          \
                               : (u16)(lr[texel & 0x1F] | lg[(texel >> 5) & 0x1F] | \
                                       lb[(texel >> 10) & 0x1F]);               \
            if (semi && (texel & 0x8000))                                       \
                *dst = blend555(*dst, m, c->abr);                               \
            else                                                                \
                *dst = (u16)(m | (texel & 0x8000));                             \
        }                                                                       \
    }

#define DEF_SPAN_TG(NAME, FETCH)                                                \
    SOFT_RASTER_IRAM static void NAME(u16* dst, int n, int32_t ui, int32_t vi, \
                                      int32_t ri, int32_t gi, int32_t bi,       \
                                      const SpanCtx* c) {                       \
        const u16 *page = c->page, *clut = c->clut;                             \
        int32_t udx = c->udx, vdx = c->vdx;                                     \
        int32_t rdx = c->rdx, gdx = c->gdx, bdx = c->bdx;                       \
        int hw_key = -1;                                                        \
        u16 hw = 0;                                                             \
        (void)clut;                                                             \
        (void)hw_key;                                                           \
        (void)hw;                                                               \
        for (; n > 0; n--, dst++, ui += udx, vi += vdx, ri += rdx, gi += gdx,   \
                      bi += bdx) {                                              \
            int u = (ui >> 16) & 0xFF, v = (vi >> 16) & 0xFF;                   \
            u16 texel;                                                          \
            FETCH;                                                              \
            if (!texel) continue;                                               \
            int r = ((texel & 0x1F) * (ri >> 16)) >> 7;                         \
            int g = (((texel >> 5) & 0x1F) * (gi >> 16)) >> 7;                  \
            int b = (((texel >> 10) & 0x1F) * (bi >> 16)) >> 7;                 \
            if (r > 31) r = 31;                                                 \
            if (g > 31) g = 31;                                                 \
            if (b > 31) b = 31;                                                 \
            if (c->semi && (texel & 0x8000))                                    \
                *dst = blend555(*dst, (u16)(r | (g << 5) | (b << 10)), c->abr); \
            else                                                                \
                *dst = (u16)(r | (g << 5) | (b << 10) | (texel & 0x8000));      \
        }                                                                       \
    }

DEF_SPAN_TF(span_tf4, TEXEL4)
DEF_SPAN_TF(span_tf8, TEXEL8)
DEF_SPAN_TF(span_tf15, TEXEL15)
DEF_SPAN_TG(span_tg4, TEXEL4)
DEF_SPAN_TG(span_tg8, TEXEL8)
DEF_SPAN_TG(span_tg15, TEXEL15)

// Sprite spans: u steps by exactly one texel per pixel. Modulation through
// the same exact tables (identity when the colour is neutral).
#define SPR_TEXEL4 texel = clut[(TEX_RD(&trow[(u & 0xFF) >> 2]) >> ((u & 3) * 4)) & 0xF]
#define SPR_TEXEL8 texel = clut[(TEX_RD(&trow[(u & 0xFF) >> 1]) >> ((u & 1) * 8)) & 0xFF]
#define SPR_TEXEL15 texel = TEX_RD(&trow[u & 0xFF])
#define DEF_SPAN_SPR(NAME, FETCH)                                               \
    SOFT_RASTER_IRAM static void NAME(u16* dst, int n, int u, const u16* trow, \
                                      const SpanCtx* c) {                       \
        const u16 *clut = c->clut, *lr = c->lr, *lg = c->lg, *lb = c->lb;       \
        int semi = c->semi;                                                     \
        (void)clut;                                                             \
        for (; n > 0; n--, dst++, u++) {                                        \
            u16 texel;                                                          \
            FETCH;                                                              \
            if (!texel) continue;                                               \
            u16 m = c->neutral ? (u16)(texel & 0x7FFF)                          \
                               : (u16)(lr[texel & 0x1F] | lg[(texel >> 5) & 0x1F] | \
                                       lb[(texel >> 10) & 0x1F]);               \
            if (semi && (texel & 0x8000))                                       \
                *dst = blend555(*dst, m, c->abr);                               \
            else                                                                \
                *dst = (u16)(m | (texel & 0x8000));                             \
        }                                                                       \
    }
// 4bpp sprite span: u advances one texel per pixel, so each halfword
// read serves up to four pixels.
SOFT_RASTER_IRAM static void span_spr4(u16* dst, int n, int u, const u16* trow,
                                       const SpanCtx* c) {
    const u16 *clut = c->clut, *lr = c->lr, *lg = c->lg, *lb = c->lb;
    int semi = c->semi;
    if (c->mclut) {
        // modulated palette: one lookup per texel
        const uint32_t* mc = c->mclut;
        while (n > 0) {
            u16 hw = TEX_RD(&trow[(u & 0xFF) >> 2]);
            int sh = u & 3;
            do {
                uint32_t e = mc[(hw >> (sh * 4)) & 0xF];
                if (e) {
                    if (semi && (e & 0x8000)) *dst = blend555(*dst, (u16)e, c->abr);
                    else *dst = (u16)e;
                }
                dst++;
                u++;
                n--;
                sh++;
            } while (sh < 4 && n > 0);
        }
        return;
    }
    while (n > 0) {
        u16 hw = TEX_RD(&trow[(u & 0xFF) >> 2]);
        int sh = u & 3;
        do {
            u16 texel = clut[(hw >> (sh * 4)) & 0xF];
            if (texel) {
                u16 m = c->neutral ? (u16)(texel & 0x7FFF)
                                   : (u16)(lr[texel & 0x1F] | lg[(texel >> 5) & 0x1F] |
                                           lb[(texel >> 10) & 0x1F]);
                if (semi && (texel & 0x8000))
                    *dst = blend555(*dst, m, c->abr);
                else
                    *dst = (u16)(m | (texel & 0x8000));
            }
            dst++;
            u++;
            n--;
            sh++;
        } while (sh < 4 && n > 0);
    }
}
DEF_SPAN_SPR(span_spr8_gen, SPR_TEXEL8)
DEF_SPAN_SPR(span_spr15_gen, SPR_TEXEL15)

// Neutral, opaque 8/15bpp rows (backdrops) that do not wrap around the
// page: texel 0 stays transparent and everything else is copied as is,
// exactly what the general loop computes with identity tables.
SOFT_RASTER_IRAM static void span_spr8(u16* dst, int n, int u, const u16* trow,
                                       const SpanCtx* c) {
    if (c->mclut && (u & 0xFF) + n <= 256) {
        // modulated palette: one lookup per texel, two texels per read
        const uint32_t* mc = c->mclut;
        int semi = c->semi;
        u &= 0xFF;
        for (; n > 0; n--, dst++, u++) {
            uint32_t e = mc[(TEX_RD(&trow[u >> 1]) >> ((u & 1) * 8)) & 0xFF];
            if (!e) continue;
            if (semi && (e & 0x8000)) *dst = blend555(*dst, (u16)e, c->abr);
            else *dst = (u16)e;
        }
        return;
    }
    if (!c->neutral || c->semi || (u & 0xFF) + n > 256) {
        span_spr8_gen(dst, n, u, trow, c);
        return;
    }
    const u16* clut = c->clut;
    u &= 0xFF;
    if (u & 1) {
        u16 t = clut[TEX_RD(&trow[u >> 1]) >> 8];
        if (t) *dst = t;
        dst++;
        u++;
        n--;
    }
    const u16* src = &trow[u >> 1];
    for (; n >= 2; n -= 2, dst += 2) {
        u16 hw = TEX_RD(src);
        src++;
        u16 a = clut[hw & 0xFF], b = clut[hw >> 8];
        if (a) dst[0] = a;
        if (b) dst[1] = b;
    }
    if (n > 0) {
        u16 t = clut[TEX_RD(src) & 0xFF];
        if (t) *dst = t;
    }
}

SOFT_RASTER_IRAM static void span_spr15(u16* dst, int n, int u, const u16* trow,
                                        const SpanCtx* c) {
    if (!c->neutral || c->semi || (u & 0xFF) + n > 256) {
        span_spr15_gen(dst, n, u, trow, c);
        return;
    }
    const u16* src = &trow[u & 0xFF];
    for (; n > 0; n--, dst++) {
        u16 t = TEX_RD(src);
        src++;
        if (t) *dst = t;
    }
}

// Axis-aligned textured sprite (SPRT/SPRT_8/SPRT_16). PSX-exact sampling:
// u advances one texel per pixel, no interpolation.
SOFT_RASTER_IRAM static void raster_sprite(const SV* v, int w, int h, u16 tpage, u16 clut,
                          int semi) {
    Tex t = tex_from(tpage, clut);
    int x0 = v->x + st.ox, y0 = v->y + st.oy;
    // clip once instead of testing every pixel
    int i0 = 0, i1 = w - 1, j0 = 0, j1 = h - 1;
    if (x0 + i0 < st.x0) i0 = st.x0 - x0;
    if (x0 + i1 > st.x1) i1 = st.x1 - x0;
    if (y0 + j0 < st.y0) j0 = st.y0 - y0;
    if (y0 + j1 > st.y1) j1 = st.y1 - y0;
    if (i0 > i1 || j0 > j1) {
        return;
    }
    if (semi && (i1 - i0 + 1) * (j1 - j0 + 1) >= 4096 && soft_semi_logged < 6) {
        soft_semi_log[soft_semi_logged][0] = t.bpp;
        soft_semi_log[soft_semi_logged][1] = i1 - i0 + 1;
        soft_semi_log[soft_semi_logged][2] = j1 - j0 + 1;
        soft_semi_log[soft_semi_logged][3] = v->r | v->g << 8 | v->b << 16;
        soft_semi_log[soft_semi_logged][4] = t.abr;
        soft_semi_logged++;
    }
    // Tilemap fast path: 4bpp, opaque, neutral modulation, u aligned to a
    // halfword. Four texels per source read, pixel pairs written as one
    // 32-bit store; texel 0 stays transparent, so the output is the same as
    // the general path's.
    bool tile = t.bpp == 0 && !semi && v->r == 0x80 && v->g == 0x80 && v->b == 0x80 &&
                ((v->u + i0) & 3) == 0 && ((i1 - i0) & 3) == 3;
    static u16 lr[32], lg[32], lb[32];
    static int lkey = -1;
    if (!tile) {
        int key = v->r | v->g << 8 | v->b << 16;
        if (key != lkey) {
            for (int c = 0; c < 32; c++) {
                lr[c] = (u16)mod5(c, v->r);
                lg[c] = (u16)(mod5(c, v->g) << 5);
                lb[c] = (u16)(mod5(c, v->b) << 10);
            }
            lkey = key;
        }
    }
    SpanCtx sc = {t.page, t.clut, 0, 0, 0, 0, 0, semi, t.abr, lr, lg, lb,
                  v->r == 0x80 && v->g == 0x80 && v->b == 0x80, 0};
    if (!tile && !sc.neutral && t.bpp <= 1 && (i1 - i0 + 1) * (j1 - j0 + 1) >= 512) {
        // Coloured palettised sprite (title/menu backdrops): modulate the
        // palette once instead of every texel. Bit 16 marks entries that are
        // drawn: transparency is decided by the raw texel, and a dark texel
        // may modulate to exactly 0.
        static uint32_t mclut[256];
        int ncol = t.bpp == 0 ? 16 : 256;
        for (int k = 0; k < ncol; k++) {
            u16 tx = t.clut[k];
            mclut[k] = tx ? (0x10000u | (tx & 0x8000) | lr[tx & 0x1F] | lg[(tx >> 5) & 0x1F] |
                             lb[(tx >> 10) & 0x1F])
                          : 0;
        }
        sc.mclut = mclut;
    }
#ifdef SOFT_ROW_FETCH
    // 8/15bpp rows (backdrops) come into SRAM in one burst each, the next
    // row fetched while this one is drawn, instead of a cache miss per four
    // or eight texels. Rows that wrap around the texture page take the
    // normal path.
    if (!tile && (t.bpp == 1 || t.bpp == 2) && v->u + i1 <= 255 && i1 - i0 >= 31) {
        static u16 rbuf[2][260] __attribute__((aligned(4)));
        int u0 = v->u + i0, u1 = v->u + i1, uoff, words;
        if (t.bpp == 2) {
            uoff = u0 & ~1;
            words = (u1 - uoff) / 2 + 1;
        } else {
            uoff = (u0 & ~3) >> 1;
            words = ((u1 >> 1) - uoff) / 2 + 1;
        }
        int cur = 0, n = i1 - i0 + 1;
        if (SOFT_ROW_FETCH(t.page + ((v->v + j0) & 0xFF) * VRAM_PITCH + uoff, rbuf[0], words)) {
            for (int j = j0; j <= j1; j++) {
                SOFT_ROW_WAIT();
                if (j < j1)
                    SOFT_ROW_FETCH(t.page + ((v->v + j + 1) & 0xFF) * VRAM_PITCH + uoff,
                                   rbuf[cur ^ 1], words);
                u16* dst = &DROW(y0 + j)[x0 + i0];
                const u16* trow = rbuf[cur] - uoff;
                if (t.bpp == 1) span_spr8(dst, n, u0, trow, &sc);
                else span_spr15(dst, n, u0, trow, &sc);
                cur ^= 1;
            }
            SOFT_ROW_WAIT();
            return;
        }
    }
#endif
    for (int j = j0; j <= j1; j++) {
        u16* row = DROW(y0 + j);
        int tv = (v->v + j) & 0xFF;
        const u16* trow = t.page + tv * VRAM_PITCH;
        if (tile) {
            u16* dst = &row[x0 + i0];
            for (int n = i0; n <= i1; n += 4) {
                u16 hw = TEX_RD(&trow[((v->u + n) & 0xFF) >> 2]);
                u16 a = t.clut[hw & 0xF];
                u16 b = t.clut[(hw >> 4) & 0xF];
                u16 c = t.clut[(hw >> 8) & 0xF];
                u16 d = t.clut[(hw >> 12) & 0xF];
                if (a && b) {
                    *(u32*)dst = (u32)a | ((u32)b << 16);
                } else {
                    if (a) dst[0] = a;
                    if (b) dst[1] = b;
                }
                if (c && d) {
                    *(u32*)(dst + 2) = (u32)c | ((u32)d << 16);
                } else {
                    if (c) dst[2] = c;
                    if (d) dst[3] = d;
                }
                dst += 4;
            }
            continue;
        }
        u16* dst = &row[x0 + i0];
        int n = i1 - i0 + 1, u = v->u + i0;
        if (t.bpp == 0) span_spr4(dst, n, u, trow, &sc);
        else if (t.bpp == 1) span_spr8(dst, n, u, trow, &sc);
        else span_spr15(dst, n, u, trow, &sc);
    }
}

// Flat rectangle (TILE family).
SOFT_RASTER_IRAM static void raster_tile(const SV* v, int w, int h, int semi, int abr) {
    int x0 = v->x + st.ox, y0 = v->y + st.oy;
    int xa = x0 < st.x0 ? st.x0 : x0;
    int xb = x0 + w - 1 > st.x1 ? st.x1 : x0 + w - 1;
    int ya = y0 < st.y0 ? st.y0 : y0;
    int yb = y0 + h - 1 > st.y1 ? st.y1 : y0 + h - 1;

    /* Opaque fill: nothing to read, nothing to interpolate, one colour for
     * every pixel -- so the general per-pixel path is all overhead. It matters
     * far more than a tile normally would, because MGS clears the frame with
     * ONE 320x224 tile: 71,680 pixels, more than half of everything the
     * rasterizer touches in a frame, each costing a branch and a single 16-bit
     * store into PSRAM. Writing pairs as one 32-bit store halves the store
     * count and lets the memory bus move a full word per transaction. Output
     * is bit-for-bit what put_flat produced. */
    if (!semi) {
        u16 c = (u16)((v->r >> 3) | ((v->g >> 3) << 5) | ((v->b >> 3) << 10));
        u32 cc = (u32)c | ((u32)c << 16);
        for (int y = ya; y <= yb; y++) {
            u16* p = DROW(y) + xa;
            int n = xb - xa + 1;
#ifdef ESP_PLATFORM
            psyz_flat_px += (unsigned)(n > 0 ? n : 0);
#endif
            if ((xa & 1) && n > 0) {   /* odd start: one halfword to align */
                *p++ = c;
                n--;
            }
            {
                u32* q = (u32*)p;
                for (int k = n >> 1; k > 0; k--) {
                    *q++ = cc;
                }
                if (n & 1) {
                    *(u16*)q = c;
                }
            }
        }
        return;
    }

    for (int y = ya; y <= yb; y++) {
        u16* row = DROW(y);
        for (int x = xa; x <= xb; x++) {
            put_flat(&row[x], v->r, v->g, v->b, semi, abr);
        }
    }
}

// Triangle with gouraud + affine UV, integer edge functions, top-left rule.
// 2D game, orthographic: affine interpolation is exact.
static unsigned tri_c_setup_end; // cycle stamp when span drawing starts (diagnostic)
unsigned soft_tri_setup_cyc, soft_tri_span_cyc, soft_tri_n;
#define CYC() (*(volatile unsigned*)0xE0001004)
SOFT_RASTER_IRAM static void raster_tri_impl(const SV* a, const SV* b, const SV* c, int textured,
                       u16 tpage, u16 clut, int semi, int gouraud);
SOFT_RASTER_IRAM static void raster_tri(const SV* a, const SV* b, const SV* c, int textured,
                       u16 tpage, u16 clut, int semi, int gouraud) {
    unsigned c0 = CYC();
    tri_c_setup_end = 0;
    raster_tri_impl(a, b, c, textured, tpage, clut, semi, gouraud);
    unsigned c1 = CYC();
    if (tri_c_setup_end) {
        soft_tri_setup_cyc += tri_c_setup_end - c0;
        soft_tri_span_cyc += c1 - tri_c_setup_end;
    } else {
        soft_tri_setup_cyc += c1 - c0;
    }
    soft_tri_n++;
}
SOFT_RASTER_IRAM static void raster_tri_impl(const SV* a, const SV* b, const SV* c, int textured,
                       u16 tpage, u16 clut, int semi, int gouraud) {
    int ax = a->x + st.ox, ay = a->y + st.oy;
    int bx = b->x + st.ox, by = b->y + st.oy;
    int cx = c->x + st.ox, cy = c->y + st.oy;

    int area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) {
        return;
    }
    // equal vertex colours: the gradient is zero, shade flat (exact)
    if (gouraud && a->r == b->r && a->r == c->r && a->g == b->g && a->g == c->g &&
        a->b == b->b && a->b == c->b) {
        gouraud = 0;
    }
    if (area < 0) { // enforce CCW so the edge functions face inward
        const SV* t = b;
        b = c;
        c = t;
        int ti = bx; bx = cx; cx = ti;
        ti = by; by = cy; cy = ti;
        area = -area;
    }

#ifdef ESP_PLATFORM
    {
        /* one-shot: where is the rasterizer actually writing, and what is it
         * clipping to? Compare against the framebuffer the scanout reads. */
        extern unsigned short g_RawVram[];
        static int shown = 3;
        if (shown > 0) {
            shown--;
            printf("[rast] vram %p g_RawVram %p ofs %d,%d clip %d,%d..%d,%d "
                   "tri %d,%d %d,%d %d,%d\n",
                   (void*)vram, (void*)g_RawVram, st.ox, st.oy,
                   st.x0, st.y0, st.x1, st.y1, ax, ay, bx, by, cx, cy);
        }
    }
#endif
    int minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
#ifdef ESP_PLATFORM
    {
        /* classify: is the world missing because it lands outside the screen,
         * or because it projects to sub-pixel size? */
        extern unsigned psyz_tri_off, psyz_tri_tiny, psyz_tri_ok;
        if (maxx < st.x0 || minx > st.x1 || maxy < st.y0 || miny > st.y1) {
            psyz_tri_off++;
        } else if ((maxx - minx) < 2 && (maxy - miny) < 2) {
            psyz_tri_tiny++;
        } else {
            static int shown = 8;
            psyz_tri_ok++;
            if (shown > 0) {
                shown--;
                printf("[ok] tri (%d,%d) (%d,%d) (%d,%d) area %d tex %d\n",
                       ax, ay, bx, by, cx, cy, area, textured);
            }
        }
    }
#endif
    /* The PSX GPU refuses any polygon wider than 1023 or taller than 511, and
     * that refusal is load-bearing rather than a curiosity. When a polygon
     * crosses behind the camera its projection is meaningless and the GTE pins
     * the offending vertices at its +/-1024 clamp, producing a quad that
     * stretches from one clamp corner across the whole screen. The console
     * simply drops those. Without the rule a software rasterizer draws them at
     * full size: on this board a searchlight cone came out as a moving white
     * sheet over the dock, with the HUD and the credit text still legible on
     * top of it because they are drawn afterwards -- three full screens of
     * texels per frame and 142 ms to raster.
     *
     * Measured on the RAW extents, before clipping to the drawing area: the
     * clip would shrink every one of them to screen size and hide exactly the
     * property being tested. */
    if ((maxx - minx) > 1023 || (maxy - miny) > 511) {
#ifdef ESP_PLATFORM
        extern unsigned psyz_tri_huge;
        psyz_tri_huge++;
#endif
        return;
    }

    if (minx < st.x0) minx = st.x0;
    if (maxx > st.x1) maxx = st.x1;
    if (miny < st.y0) miny = st.y0;
    if (maxy > st.y1) maxy = st.y1;
    if (minx > maxx || miny > maxy) {
        return;
    }

    // edge setup: E(x,y) >= 0 inside, top-left edges get the <0 bias on the
    // others so shared edges paint exactly once
    int A0 = by - cy, B0 = cx - bx, C0 = bx * cy - by * cx;
    int A1 = cy - ay, B1 = ax - cx, C1 = cx * ay - cy * ax;
    int A2 = ay - by, B2 = bx - ax, C2 = ax * by - ay * bx;
    int bias0 = (A0 > 0 || (A0 == 0 && B0 > 0)) ? 0 : -1;
    int bias1 = (A1 > 0 || (A1 == 0 && B1 > 0)) ? 0 : -1;
    int bias2 = (A2 > 0 || (A2 == 0 && B2 > 0)) ? 0 : -1;

    Tex t;
    if (textured) {
        t = tex_from(tpage, clut);
    }

    int w0row = A0 * minx + B0 * miny + C0 + bias0;
    int w1row = A1 * minx + B1 * miny + C1 + bias1;
    int w2row = A2 * minx + B2 * miny + C2 + bias2;

    // Per-triangle attribute gradients, 16.16 fixed point. Xtensa has no fast
    // divider, and the first-correctness version paid 3-5 integer divisions
    // per PIXEL for barycentric attributes - measured at 100-130 cy/px on the
    // S3, it dominated every gouraud primitive. An affine attribute is a plane
    // over (x,y): compute d/dx and d/dy once with a handful of divisions, then
    // step. The +area/2 rounds the gradient to nearest, which keeps the last
    // scanline of a 255-wide gradient from drifting versus the exact form.
#define GRAD(p0, p1, p2)                                                           dx_ = ((s64)(A0) * (p0) + (s64)(A1) * (p1) + (s64)(A2) * (p2)) * 65536;        dy_ = ((s64)(B0) * (p0) + (s64)(B1) * (p1) + (s64)(B2) * (p2)) * 65536;        dx_ = dx_ >= 0 ? (dx_ + area / 2) / area : (dx_ - area / 2) / area;            dy_ = dy_ >= 0 ? (dy_ + area / 2) / area : (dy_ - area / 2) / area;

    typedef long long s64;
    s64 dx_, dy_;
    int e0r = w0row - bias0, e1r = w1row - bias1, e2r = w2row - bias2;

    // Accumulators are 32-bit 16.16: attributes are 8-bit, spans are under
    // 1024px, so values stay far from overflow - and on a 32-bit Xtensa a
    // 64-bit accumulator costs two registers and multi-instruction adds per
    // pixel, which measured SLOWER than the hardware-divide version it was
    // meant to replace (17.8M vs 13.1M cy on the quad battery).
    typedef int32_t fx;
    fx rrow = 0, grow = 0, brow = 0, urow = 0, vrow = 0;
    fx rdx = 0, rdy = 0, gdx = 0, gdy = 0, bdx = 0, bdy = 0;
    fx udx = 0, udy = 0, vdx = 0, vdy = 0;
    if (gouraud) {
        GRAD(a->r, b->r, c->r) rdx = (fx)dx_; rdy = (fx)dy_;
        GRAD(a->g, b->g, c->g) gdx = (fx)dx_; gdy = (fx)dy_;
        GRAD(a->b, b->b, c->b) bdx = (fx)dx_; bdy = (fx)dy_;
        rrow = (fx)(((s64)(e0r * a->r + e1r * b->r + e2r * c->r) << 16) / area);
        grow = (fx)(((s64)(e0r * a->g + e1r * b->g + e2r * c->g) << 16) / area);
        brow = (fx)(((s64)(e0r * a->b + e1r * b->b + e2r * c->b) << 16) / area);
    }
    if (textured) {
        GRAD(a->u, b->u, c->u) udx = (fx)dx_; udy = (fx)dy_;
        GRAD(a->v, b->v, c->v) vdx = (fx)dx_; vdy = (fx)dy_;
        urow = (fx)(((s64)(e0r * a->u + e1r * b->u + e2r * c->u) << 16) / area);
        vrow = (fx)(((s64)(e0r * a->v + e1r * b->v + e2r * c->v) << 16) / area);
    }
#undef GRAD

    // Fast path for the dominant WRP case: opaque untextured spans (the big
    // gouraud background quads). Span bounds come from the edge functions
    // directly (three divides per row on the LX7's hardware divider), the
    // inner loop steps only RGB and writes pixel pairs with 32-bit stores.
    // Attribute stepping is identical to the generic loop, so pixel output
    // is bit-exact with it.
    if (!textured && !semi) {
        fx r0 = gouraud ? rrow : (fx)(a->r << 16);
        fx g0 = gouraud ? grow : (fx)(a->g << 16);
        fx b0 = gouraud ? brow : (fx)(a->b << 16);
        fx frdx = gouraud ? rdx : 0, frdy = gouraud ? rdy : 0;
        fx fgdx = gouraud ? gdx : 0, fgdy = gouraud ? gdy : 0;
        fx fbdx = gouraud ? bdx : 0, fbdy = gouraud ? bdy : 0;
        for (int y = miny; y <= maxy; y++, w0row += B0, w1row += B1,
                 w2row += B2, r0 += frdy, g0 += fgdy, b0 += fbdy) {
            int xl = minx, xr = maxx;
            int we[3] = {w0row, w1row, w2row};
            int Ae[3] = {A0, A1, A2};
            int empty = 0;
            for (int e = 0; e < 3; e++) {
                int w = we[e], A = Ae[e];
                if (A == 0) {
                    if (w < 0) { empty = 1; break; }
                } else if (A > 0) {
                    if (w < 0) {
                        int x = minx + (-w + A - 1) / A;
                        if (x > xl) xl = x;
                    }
                } else {
                    if (w < 0) { empty = 1; break; }
                    int x = minx + w / (-A);
                    if (x < xr) xr = x;
                }
            }
            if (empty || xl > xr) {
                continue;
            }
            fx ri = r0 + (fx)(xl - minx) * frdx;
            fx gi = g0 + (fx)(xl - minx) * fgdx;
            fx bi = b0 + (fx)(xl - minx) * fbdx;
            u16* p = DROW(y) + xl;
            int n = xr - xl + 1;
#define SPAN_PIX()                                                             \
    (u16)((((unsigned)ri >> 19) & 0x1F) | ((((unsigned)gi >> 19) & 0x1F) << 5) \
          | ((((unsigned)bi >> 19) & 0x1F) << 10))
            if (((uintptr_t)p & 2) && n) {
                *p++ = SPAN_PIX();
                ri += frdx; gi += fgdx; bi += fbdx;
                n--;
            }
            u32* p2 = (u32*)p;
            while (n >= 2) {
                u16 lo = SPAN_PIX();
                ri += frdx; gi += fgdx; bi += fbdx;
                u16 hi = SPAN_PIX();
                ri += frdx; gi += fgdx; bi += fbdx;
                *p2++ = (u32)lo | ((u32)hi << 16);
                n -= 2;
            }
            if (n) {
                *(u16*)p2 = SPAN_PIX();
            }
#undef SPAN_PIX
        }
        return;
    }

    tri_c_setup_end = CYC();
    // Textured, one colour (scaled scenery, clouds, rotated
    // sprites): the generic loop below pays tex_fetch's switch plus three
    // multiply-clamps per pixel. Same spans and UV stepping, so the output
    // is identical; modulation comes from per-primitive tables holding
    // exactly mod5(c, col).
    if (textured && !gouraud) {
        static u16 lr[32], lg[32], lb[32];
        static int lkey = -1;
        int key = a->r | a->g << 8 | a->b << 16;
        if (key != lkey) {
            for (int c = 0; c < 32; c++) {
                lr[c] = (u16)mod5(c, a->r);
                lg[c] = (u16)(mod5(c, a->g) << 5);
                lb[c] = (u16)(mod5(c, a->b) << 10);
            }
            lkey = key;
        }
        SpanCtx sc = {t.page, t.clut, udx, vdx, 0, 0, 0, semi, t.abr, lr, lg, lb,
                      a->r == 0x80 && a->g == 0x80 && a->b == 0x80, 0};
        for (int y = miny; y <= maxy; y++, w0row += B0, w1row += B1,
                 w2row += B2, urow += udy, vrow += vdy) {
            int xl = minx, xr = maxx;
            int we[3] = {w0row, w1row, w2row};
            int Ae[3] = {A0, A1, A2};
            int empty = 0;
            for (int e = 0; e < 3; e++) {
                int w = we[e], A = Ae[e];
                if (A == 0) {
                    if (w < 0) { empty = 1; break; }
                } else if (A > 0) {
                    if (w < 0) {
                        int x = minx + (-w + A - 1) / A;
                        if (x > xl) xl = x;
                    }
                } else {
                    if (w < 0) { empty = 1; break; }
                    int x = minx + w / (-A);
                    if (x < xr) xr = x;
                }
            }
            if (empty || xl > xr) {
                continue;
            }
            soft_px += (unsigned)(xr - xl + 1);
            fx ui = urow + (fx)(xl - minx) * udx;
            fx vi = vrow + (fx)(xl - minx) * vdx;
            u16* row = DROW(y) + xl;
            int n = xr - xl + 1;
            if (t.bpp == 0) span_tf4(row, n, ui, vi, &sc);
            else if (t.bpp == 1) span_tf8(row, n, ui, vi, &sc);
            else span_tf15(row, n, ui, vi, &sc);
        }
        return;
    }

    // Textured + gouraud (lit scenery): same spans and attribute stepping as
    // the generic loop, with the texel fetch specialised per bpp and the
    // modulation inlined, so the output is identical.
    if (textured && gouraud) {
        SpanCtx sc = {t.page, t.clut, udx, vdx, rdx, gdx, bdx, semi, t.abr, 0, 0, 0, 0, 0};
        for (int y = miny; y <= maxy; y++, w0row += B0, w1row += B1,
                 w2row += B2, rrow += rdy, grow += gdy, brow += bdy,
                 urow += udy, vrow += vdy) {
            int xl = minx, xr = maxx;
            int we[3] = {w0row, w1row, w2row};
            int Ae[3] = {A0, A1, A2};
            int empty = 0;
            for (int e = 0; e < 3; e++) {
                int w = we[e], A = Ae[e];
                if (A == 0) {
                    if (w < 0) { empty = 1; break; }
                } else if (A > 0) {
                    if (w < 0) {
                        int x = minx + (-w + A - 1) / A;
                        if (x > xl) xl = x;
                    }
                } else {
                    if (w < 0) { empty = 1; break; }
                    int x = minx + w / (-A);
                    if (x < xr) xr = x;
                }
            }
            if (empty || xl > xr) {
                continue;
            }
            soft_px += (unsigned)(xr - xl + 1);
            fx d = (fx)(xl - minx);
            fx ri = rrow + d * rdx, gi = grow + d * gdx, bi = brow + d * bdx;
            fx ui = urow + d * udx, vi = vrow + d * vdx;
            u16* row = DROW(y) + xl;
            int n = xr - xl + 1;
            if (t.bpp == 0) span_tg4(row, n, ui, vi, ri, gi, bi, &sc);
            else if (t.bpp == 1) span_tg8(row, n, ui, vi, ri, gi, bi, &sc);
            else span_tg15(row, n, ui, vi, ri, gi, bi, &sc);
        }
        return;
    }

    // Generic loop, span-based: per row the edge functions give [xl, xr]
    // directly (three hardware divides), so the inner loop carries no edge
    // accumulators and no accept test - just attribute stepping + the pixel
    // write. Identical stepping start (xl offset from minx) keeps output
    // bit-exact with the per-pixel-test form.
    for (int y = miny; y <= maxy; y++, w0row += B0, w1row += B1, w2row += B2,
             rrow += rdy, grow += gdy, brow += bdy, urow += udy, vrow += vdy) {
        int xl = minx, xr = maxx;
        {
            int we[3] = {w0row, w1row, w2row};
            int Ae[3] = {A0, A1, A2};
            int empty = 0;
            for (int e = 0; e < 3; e++) {
                int w = we[e], A = Ae[e];
                if (A == 0) {
                    if (w < 0) { empty = 1; break; }
                } else if (A > 0) {
                    if (w < 0) {
                        int x = minx + (-w + A - 1) / A;
                        if (x > xl) xl = x;
                    }
                } else {
                    if (w < 0) { empty = 1; break; }
                    int x = minx + w / (-A);
                    if (x < xr) xr = x;
                }
            }
            if (empty || xl > xr) {
                continue;
            }
        }
        fx ri = rrow + (fx)(xl - minx) * rdx;
        fx gi = grow + (fx)(xl - minx) * gdx;
        fx bi = brow + (fx)(xl - minx) * bdx;
        fx ui = urow + (fx)(xl - minx) * udx;
        fx vi = vrow + (fx)(xl - minx) * vdx;
        u16* row = DROW(y);
        for (int x = xl; x <= xr; x++, ri += rdx, gi += gdx, bi += bdx,
                 ui += udx, vi += vdx) {
            int r8 = a->r, g8 = a->g, b8 = a->b;
            if (gouraud) {
                r8 = (int)(ri >> 16);
                g8 = (int)(gi >> 16);
                b8 = (int)(bi >> 16);
            }
            if (textured) {
                put_texel(&row[x],
                          tex_fetch(&t, (int)(ui >> 16), (int)(vi >> 16)), r8,
                          g8, b8, semi, t.abr);
            } else {
                put_flat(&row[x], r8, g8, b8, semi, (cur_tpage >> 5) & 3);
            }
        }
    }
}

// Gouraud line, Bresenham over the major axis.
SOFT_RASTER_IRAM static void raster_line(const SV* a, const SV* b, int semi) {
    int x0 = a->x + st.ox, y0 = a->y + st.oy;
    int x1 = b->x + st.ox, y1 = b->y + st.oy;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int steps = (dx > -dy ? dx : -dy);
    if (steps == 0) {
        steps = 1;
    }
    int abr = (cur_tpage >> 5) & 3;
    for (int i = 0;; i++) {
        if (x0 >= st.x0 && x0 <= st.x1 && y0 >= st.y0 && y0 <= st.y1) {
            int r = a->r + (b->r - a->r) * i / steps;
            int g = a->g + (b->g - a->g) * i / steps;
            int bl = a->b + (b->b - a->b) * i / steps;
            put_flat(&DROW(y0)[x0], r, g, bl, semi, abr);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// ---- VRAM block operations -------------------------------------------------

// A corrupt packet must degrade to a bounded no-op, not a multi-second (or
// unbounded) VRAM stomp - clamp every block op to physical VRAM limits.
static inline int vram_clamp_w(const PS1_RECT* rect, const char* op) {
    int w = rect->w;
    if (w < 0 || w > VRAM_W) {
        WARNF("%s rect suspicious: %d,%d %dx%d", op, rect->x, rect->y,
              rect->w, rect->h);
        w = w < 0 ? 0 : VRAM_W;
    }
    return w;
}
static inline int vram_clamp_h(const PS1_RECT* rect) {
    int h = rect->h;
    return h < 0 ? 0 : (h > VRAM_H ? VRAM_H : h);
}

static void soft_clear_image(const PS1_RECT* rect, u8 r, u8 g, u8 b) {
    clut_cache_flush();
    u16 c = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    int w = vram_clamp_w(rect, "clear"), h = vram_clamp_h(rect);
    for (int y = 0; y < h; y++) {
        u16* row = vram + ((rect->y + y) & (VRAM_H - 1)) * VRAM_PITCH;
        for (int x = 0; x < w; x++) {
            row[(rect->x + x) & (VRAM_W - 1)] = c;
        }
    }
}

static void soft_load_image(const PS1_RECT* rect, const u16* src) {
    if (!src) {
        // The PSX's DMA read open bus at address 0 and uploaded garbage
        // without complaint -- MGS's menu does exactly this on purpose
        // (menu_restore_nouse runs before menu_init_nouse fills the panel
        // in). A host dereference of NULL is fatal, so skip the upload; the
        // panel is overwritten with real pixels moments later either way.
        WARNF("load image from NULL (%d,%d %dx%d) skipped", rect->x, rect->y,
              rect->w, rect->h);
        return;
    }
    clut_cache_flush();
    int w = vram_clamp_w(rect, "load"), h = vram_clamp_h(rect);
    (void)w;
    for (int y = 0; y < h; y++) {
        u16* dst = vram + ((rect->y + y) & (VRAM_H - 1)) * VRAM_PITCH + rect->x;
        memcpy(dst, src, (size_t)rect->w * 2);
        src += rect->w;
    }
}

static void soft_store_image(const PS1_RECT* rect, u16* dst) {
    int w = vram_clamp_w(rect, "store"), h = vram_clamp_h(rect);
    (void)w;
    for (int y = 0; y < h; y++) {
        const u16* src =
            vram + ((rect->y + y) & (VRAM_H - 1)) * VRAM_PITCH + rect->x;
        memcpy(dst, src, (size_t)rect->w * 2);
        dst += rect->w;
    }
}

static void soft_move_image(const PS1_RECT* rect, int dx, int dy) {
    clut_cache_flush();
    int w = vram_clamp_w(rect, "move"), h = vram_clamp_h(rect);
    // clip so both source and destination stay inside VRAM
    int maxx = rect->x > dx ? rect->x : dx;
    int maxy = rect->y > dy ? rect->y : dy;
    if (w > VRAM_W - maxx) w = VRAM_W - maxx;
    if (h > VRAM_H - maxy) h = VRAM_H - maxy;
    if (w <= 0 || h <= 0 || rect->x < 0 || rect->y < 0 || dx < 0 || dy < 0) {
        return;
    }
    // memmove per row, walking in the safe direction for overlap
    if (dy <= rect->y) {
        for (int y = 0; y < h; y++) {
            memmove(vram + (dy + y) * VRAM_PITCH + dx,
                    vram + (rect->y + y) * VRAM_PITCH + rect->x, (size_t)w * 2);
        }
    } else {
        for (int y = h - 1; y >= 0; y--) {
            memmove(vram + (dy + y) * VRAM_PITCH + dx,
                    vram + (rect->y + y) * VRAM_PITCH + rect->x, (size_t)w * 2);
        }
    }
}



// ---- packet parsing into core calls ---------------------------------------
// Mirrors sdl3_gl.c's Draw_PushPrim contract exactly: same s11 decoding, same
// word accounting, same quad split (0,1,2)+(1,3,2).

static int soft_push_prim(u_long* packets, int max_len) {
    int len = max_len;
    int code = (int)(*packets >> 24) & 0xFF;
    bool isPoly = !(code & 0x40);
    bool isLine = (code & 0x40) && !(code & 0x20);
    bool isTile = (code & 0x40) && (code & 0x20);
    bool isTextured = (code & TEXTURED) != 0;
    bool isGouraud = (code & GOURAUD) != 0;
    bool isShadeTex = !((code & 1) && isTextured && !isLine);
    bool isSemi = (code & SEMITRANSP) != 0;
    u16 tpage = 0, clut = 0;
    SV v[4];

    int r0 = 0x80, g0 = 0x80, b0 = 0x80;
    if (isShadeTex) {
        r0 = (u8)(*packets >> 0);
        g0 = (u8)(*packets >> 8);
        b0 = (u8)(*packets >> 16);
    }
    packets++;
    len--;

    if (isPoly) {
        if (!(code & TRIANGLE)) {
            WARNF("code %02X not supported", code);
            return max_len - len;
        }
        int n = (code & EXTRA_VERTEX) ? 4 : 3;
        for (int i = 0; i < n; i++) {
            if (i == 0 || !isGouraud) {
                v[i].r = r0;
                v[i].g = g0;
                v[i].b = b0;
            }
            if (i > 0 && isGouraud && len > 0) {
                v[i].r = ((u8*)packets)[0];
                v[i].g = ((u8*)packets)[1];
                v[i].b = ((u8*)packets)[2];
                packets++;
                len--;
            }
            if (len <= 0) {
                return max_len - len;
            }
            v[i].x = s11(((short*)packets)[0]);
            v[i].y = s11(((short*)packets)[1]);
            packets++;
            len--;
            if (isTextured && len > 0) {
                v[i].u = ((u8*)packets)[0];
                v[i].v = ((u8*)packets)[1];
                if (i == 0) {
                    clut = ((u16*)packets)[1];
                } else if (i == 1) {
                    tpage = ((u16*)packets)[1];
                }
                packets++;
                len--;
            } else {
                v[i].u = v[i].v = 0;
            }
        }
        if (!isTextured) {
            tpage = cur_tpage;
        }
        if (!isShadeTex) {
            // raw-texture polys: neutral modulation
            for (int i = 0; i < n; i++) {
                v[i].r = v[i].g = v[i].b = 0x80;
            }
        }
        // Axis-aligned textured quad with 1:1 texel mapping and one colour
        // (stage name banner, dissolve strips, HUD panels): identical output
        // to the two-triangle split, but the sprite loop skips the edge and
        // attribute stepping and reads 4bpp texels a halfword at a time.
        if (n == 4 && isTextured && v[0].y == v[1].y && v[2].y == v[3].y &&
            v[0].x == v[2].x && v[1].x == v[3].x && v[0].u == v[2].u &&
            v[1].u == v[3].u && v[0].v == v[1].v && v[2].v == v[3].v &&
            v[1].x > v[0].x && v[2].y > v[0].y &&
            v[1].u - v[0].u == v[1].x - v[0].x &&
            v[2].v - v[0].v == v[2].y - v[0].y &&
            (!isGouraud ||
             (v[1].r == v[0].r && v[1].g == v[0].g && v[1].b == v[0].b &&
              v[2].r == v[0].r && v[2].g == v[0].g && v[2].b == v[0].b &&
              v[3].r == v[0].r && v[3].g == v[0].g && v[3].b == v[0].b))) {
            raster_sprite(&v[0], v[1].x - v[0].x, v[2].y - v[0].y, tpage, clut,
                          isSemi);
            return max_len - len;
        }
        raster_tri(&v[0], &v[1], &v[2], isTextured, tpage, clut, isSemi,
                   isGouraud);
        if (n == 4) {
            raster_tri(&v[1], &v[3], &v[2], isTextured, tpage, clut, isSemi,
                       isGouraud);
        }
    } else if (isLine) {
        bool padding = true;
        int nPoints = ((code >> 2) & 3) + 1;
        if (nPoints == 1) {
            padding = false;
            nPoints++;
        }
        SV pts[4];
        int parsed = 0;
        pts[0].r = r0;
        pts[0].g = g0;
        pts[0].b = b0;
        for (int i = 0; len > 0 && i < nPoints; i++) {
            pts[i].x = s11(((short*)packets)[0]);
            pts[i].y = s11(((short*)packets)[1]);
            packets++;
            len--;
            parsed = i + 1;
            if (len > 0 && i + 1 < nPoints && isGouraud) {
                pts[i + 1].r = ((u8*)packets)[0];
                pts[i + 1].g = ((u8*)packets)[1];
                pts[i + 1].b = ((u8*)packets)[2];
                packets++;
                len--;
            }
        }
        if (!isGouraud) {
            for (int i = 1; i < parsed; i++) {
                pts[i].r = pts[0].r;
                pts[i].g = pts[0].g;
                pts[i].b = pts[0].b;
            }
        }
        for (int i = 0; i + 1 < parsed; i++) {
            raster_line(&pts[i], &pts[i + 1], isSemi);
        }
        if (padding && len > 0) {
            packets++;
            len--;
        }
    } else if (isTile) {
        int w = 0, h = 0;
        v[0].r = r0;
        v[0].g = g0;
        v[0].b = b0;
        v[0].x = s11(((short*)packets)[0]);
        v[0].y = s11(((short*)packets)[1]);
        packets++;
        len--;
        if (isTextured) {
            v[0].u = ((u8*)packets)[0];
            v[0].v = ((u8*)packets)[1];
            clut = ((u16*)packets)[1];
            tpage = cur_tpage;
            packets++;
            len--;
        }
        switch (code & ~3) {
        case 0x60: // TILE
        case 0x64: // SPRT
            w = ((s16*)packets)[0];
            h = ((s16*)packets)[1];
            packets++;
            len--;
            break;
        case 0x68:
        case 0x6C:
            w = h = 1;
            break;
        case 0x70:
        case 0x74:
            w = h = 8;
            break;
        case 0x78:
        case 0x7C:
            w = h = 16;
            break;
        default:
            break;
        }
        if (isTextured) {
            raster_sprite(&v[0], w, h, tpage, clut, isSemi);
        } else {
            raster_tile(&v[0], w, h, isSemi, (cur_tpage >> 5) & 3);
        }
    }
    return max_len - len;
}

// ---- Draw_* state ----------------------------------------------------------

static void soft_set_area_start(int x, int y) {
    st.x0 = x;
    st.y0 = y;
}
static void soft_set_area_end(int x, int y) {
    st.x1 = x;
    st.y1 = y;
}
static void soft_set_offset(int x, int y) {
    x = x % VRAM_W;
    y = y % VRAM_H;
    if (x < 0) {
        x += VRAM_W;
    }
    if (y < 0) {
        y += VRAM_H;
    }
    st.ox = x;
    st.oy = y;
}

