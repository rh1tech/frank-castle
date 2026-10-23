/*
 * frank-castle — psyz platform backend for RP2350: lifecycle, log, pads,
 * audio lock, memory card files.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <psyz.h>
#include <psyz/input.h>
#include <psyz/log.h>
#include <psyz/spu.h>
#include <kernel.h>
#include <libetc.h>
#include <romio.h>

#include "ff.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

#ifndef PSYZ_LOG_LEVEL
#define PSYZ_LOG_LEVEL 1 /* 0 debug, 1 info, 2 warn, 3 error */
#endif

void psyz_log(unsigned int level, const char *file, unsigned int line, const char *func,
              const char *fmt, ...) {
    static const char levels[] = "DIWE";
    if (level < PSYZ_LOG_LEVEL || level >= sizeof(levels) - 1) return;
    va_list args;
    va_start(args, fmt);
    printf("[%c][%s:%u][%s] ", levels[level], file, line, func);
    vprintf(fmt, args);
    putchar('\n');
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* Game heap: big blocks (whole files, buffers) in PSRAM, small in SRAM */
/* ------------------------------------------------------------------ */

#undef malloc
#undef calloc
#undef realloc
#undef free
/* stdlib.h was read with the castle_* renames in effect: declare the real ones */
extern void *malloc(size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern void *psram_malloc(size_t size);
extern void *psram_realloc(void *ptr, size_t size);
extern void psram_free(void *ptr);

#define PSRAM_ALLOC_MIN 4096

static inline bool in_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x11000000u && a < 0x11800000u;
}

void *castle_malloc(size_t n) {
    void *p = NULL;
    if (n < PSRAM_ALLOC_MIN) p = malloc(n);
    if (!p) p = psram_malloc(n);
    if (!p) printf("castle_malloc(%u) failed\n", (unsigned)n);
    return p;
}

void *castle_calloc(size_t n, size_t sz) {
    void *p = castle_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void castle_free(void *p) {
    if (!p) return;
    if (in_psram(p))
        psram_free(p);
    else
        free(p);
}

void *castle_realloc(void *p, size_t n) {
    if (!p) return castle_malloc(n);
    if (in_psram(p)) return psram_realloc(p, n);
    void *q = n < PSRAM_ALLOC_MIN ? realloc(p, n) : NULL;
    if (q) return q;
    q = psram_malloc(n);
    if (q) {
        memcpy(q, p, n); /* old block is at least as large or smaller: copy n */
        free(p);
    }
    return q;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

extern void PsxVideo_Init(void);
static bool is_init;

bool InitPlatform(void) {
    if (is_init) return true;
    PsxVideo_Init();
    is_init = true;
    return true;
}

void ResetPlatform(void) {}

/* ------------------------------------------------------------------ */
/* Audio lock. The SPU state is shared between the game and the mixer. */
/* ------------------------------------------------------------------ */

/* SPU lock shared by the game (core 0) and the mixer (core 1). A plain SRAM
 * word taken with compare-and-swap: unlike a pico mutex it never masks
 * interrupts, which on core 1 would delay the HSTX IRQ and desync HDMI.
 * Core 0 may nest (the game calls SPU code from SPU code); core 1 only tries. */
static volatile uint32_t spu_owner; /* 0 free, else core number + 1 */
static uint32_t spu_depth;          /* nesting depth of the owner */
volatile bool psyz_audio_ready;     /* the mixer on core 1 waits for this */

static bool spu_try(uint32_t me) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&spu_owner, &expected, me, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        spu_depth = 1;
        return true;
    }
    return false;
}

/* re-entrant on the owning core: SPU code calls SPU code (the mixer pulls
 * CD audio through Psyz_CdPullSamples, which locks again) */
void Psyz_AudioLock(void) {
    uint32_t me = get_core_num() + 1;
    if (spu_owner == me) {
        spu_depth++;
        return;
    }
    while (!spu_try(me)) tight_loop_contents();
}

void Psyz_AudioUnlock(void) {
    if (--spu_depth) return;
    __atomic_store_n(&spu_owner, 0, __ATOMIC_RELEASE);
}

/* core 1 mixer: never block (scanout work shares that core) */
bool Psyz_AudioTryLock(void) {
    uint32_t me = get_core_num() + 1;
    if (spu_owner == me) {
        spu_depth++;
        return true;
    }
    return spu_try(me);
}

int Psyz_AudioInit(void) {
    Psyz_AudioLock();
    Psyz_SpuInit();
    Psyz_AudioUnlock();
    psyz_audio_ready = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pads: USB keyboard -> PS1 digital pad on port 1                     */
/* ------------------------------------------------------------------ */

extern int usbhid_get_key_action(uint8_t *keycode, int *down);

static uint8_t key_down[256 / 8];

static const struct {
    uint8_t hid;
    uint16_t pad;
} key_map[] = {
    {0x52, PADLup},    {0x51, PADLdown}, {0x50, PADLleft}, {0x4F, PADLright},
    {0x1B, PADRdown},  /* X: cross (jump) */
    {0x1D, PADRleft},  /* Z: square */
    {0x06, PADRright}, /* C: circle */
    {0x16, PADRup},    /* S: triangle */
    {0x04, PADL1},     /* A */
    {0x07, PADR1},     /* D */
    {0x14, PADL2},     /* Q */
    {0x08, PADR2},     /* E */
    {0x28, PADstart},  /* Enter */
    {0x2B, PADselect}, /* Tab */
    {0x2C, PADselect}, /* Space */
};

/* Debug UART input: every received byte presses a button for UART_HOLD
 * frames; sending it again keeps it held (tools/uart.py drives this). */
#define UART_HOLD 6
static uint8_t uart_hold[16];
static const struct {
    char c;
    uint8_t bit;
} uart_map[] = {
    {'w', 12}, {'s', 14}, {'a', 15}, {'d', 13}, /* d-pad */
    {'x', 6},  {'z', 7},  {'c', 5},  {'v', 4},  /* cross square circle triangle */
    {'q', 2},  {'e', 3},  {'1', 0},  {'3', 1},  /* L1 R1 L2 R2 */
    {'\r', 11}, {'\n', 11}, {'p', 11},          /* start */
    {' ', 8},  {'o', 8},                         /* select */
};

/* "~C pad BITS FRAMES~" (tools/castle_cmd.py): hold exactly these pad bits
 * for that many frames */
static unsigned pad_cmd_bits, pad_cmd_frames;
void platform_pad_hold(unsigned bits, unsigned frames) {
    pad_cmd_bits = bits;
    pad_cmd_frames = frames;
}

static unsigned uart_pad_bits(void) {
    int ch;
    static bool in_tilde; /* "~...~" is a command, not key presses */
    extern void platform_uart_cmd(int ch);
    while ((ch = getchar_timeout_us(0)) >= 0) {
        platform_uart_cmd(ch);
        if (ch == '~') in_tilde = !in_tilde;
        if (in_tilde || ch == '~') continue;
        for (unsigned i = 0; i < sizeof(uart_map) / sizeof(uart_map[0]); i++)
            if (uart_map[i].c == (char)ch) uart_hold[uart_map[i].bit] = UART_HOLD;
    }
    unsigned bits = 0;
    if (pad_cmd_frames) {
        pad_cmd_frames--;
        bits = pad_cmd_bits;
    }
    for (int b = 0; b < 16; b++)
        if (uart_hold[b]) {
            uart_hold[b]--;
            bits |= 1u << b;
        }
    return bits;
}

void MyPadInit(int mode) { (void)mode; }

void Psyz_PadsPoll(void) {
    uint8_t code;
    int down;
    while (usbhid_get_key_action(&code, &down)) {
        if (down)
            key_down[code >> 3] |= (uint8_t)(1u << (code & 7));
        else
            key_down[code >> 3] &= (uint8_t)~(1u << (code & 7));
    }
    unsigned pressed = uart_pad_bits();
    for (unsigned i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++)
        if (key_down[key_map[i].hid >> 3] & (1u << (key_map[i].hid & 7))) pressed |= key_map[i].pad;

    char frame[PSYZ_PAD_BUF_LEN];
    memset(frame, 0, sizeof(frame));
    frame[1] = (char)PSYZ_CTRL_DIGITAL_PAD;
    frame[2] = (char)(~(pressed >> 8) & 0xFF);
    frame[3] = (char)(~pressed & 0xFF);
    Psyz_PadsSet(0, frame, sizeof(frame));
    memset(frame, 0xFF, sizeof(frame));
    Psyz_PadsSet(1, frame, sizeof(frame));
}

/* ------------------------------------------------------------------ */
/* Files: memory cards ("bu00:NAME") as files in /castle/bu00/         */
/* ------------------------------------------------------------------ */

#define DATA_DIR "/castle"
#define MAX_FDS 4

static FIL fds[MAX_FDS];
static bool fd_used[MAX_FDS];

/* "bu00:BASLUS-00067DRAX00" -> "/castle/bu00/BASLUS-00067DRAX00" */
static void map_path(char *dst, const char *src, size_t len) {
    if (strncmp(src, "bu", 2) == 0 && src[4] == ':') {
        char dir[32];
        snprintf(dir, sizeof(dir), DATA_DIR "/%.4s", src);
        f_mkdir(dir);
        const char *name = src + 5;
        if (*name == '*') name = "";
        snprintf(dst, len, "%s/%s", dir, name);
    } else {
        snprintf(dst, len, DATA_DIR "/%s", src);
    }
}

int psyz_open(const char *devname, int flag) {
    char path[128];
    map_path(path, devname, sizeof(path));
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_used[i]) continue;
        BYTE mode = 0;
        if (flag & FREAD) mode |= FA_READ;
        if (flag & FWRITE) mode |= FA_WRITE;
        if (flag & FCREAT) mode |= (flag & FTRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
        if (!mode) mode = FA_READ;
        if (f_open(&fds[i], path, mode) != FR_OK) return -1;
        fd_used[i] = true;
        return i + 3;
    }
    return -1;
}

static FIL *fd_get(long fd) {
    long i = fd - 3;
    return (i >= 0 && i < MAX_FDS && fd_used[i]) ? &fds[i] : NULL;
}

int psyz_close(int fd) {
    FIL *f = fd_get(fd);
    if (!f) return -1;
    f_close(f);
    fd_used[fd - 3] = false;
    return 0;
}

unsigned long psyz_lseek(int fd, unsigned int offset, int whence) {
    FIL *f = fd_get(fd);
    if (!f) return (unsigned long)-1;
    FSIZE_t pos = offset;
    if (whence == 1) pos += f_tell(f);
    if (whence == 2) pos += f_size(f);
    f_lseek(f, pos);
    return (unsigned long)f_tell(f);
}

long psyz_read(long fd, void *buf, long n) {
    FIL *f = fd_get(fd);
    UINT got = 0;
    if (!f || f_read(f, buf, (UINT)n, &got) != FR_OK) return -1;
    return (long)got;
}

int psyz_write(int fd, char *buf, int n) {
    FIL *f = fd_get(fd);
    UINT put = 0;
    if (!f || f_write(f, buf, (UINT)n, &put) != FR_OK) return -1;
    f_sync(f);
    return (int)put;
}

long psyz_ioctl(int fd, int com, int arg) {
    (void)fd;
    (void)com;
    (void)arg;
    return 0;
}

/* Directory listing for the save menu */
static FFDIR search_dir;
static bool search_open;

static struct DIRENTRY *next_entry(struct DIRENTRY *out) {
    FILINFO fi;
    while (search_open && f_readdir(&search_dir, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        memset(out, 0, sizeof(*out));
        strncpy(out->name, fi.fname, sizeof(out->name) - 1);
        out->attr = 0x10 | 0x40;
        out->size = (int)fi.fsize;
        return out;
    }
    if (search_open) f_closedir(&search_dir);
    search_open = false;
    return NULL;
}

struct DIRENTRY *my_firstfile(char *dirPath, struct DIRENTRY *firstEntry) {
    char path[128];
    map_path(path, dirPath, sizeof(path));
    size_t n = strlen(path);
    if (n && path[n - 1] == '/') path[n - 1] = 0;
    if (search_open) f_closedir(&search_dir);
    search_open = f_opendir(&search_dir, path) == FR_OK;
    return next_entry(firstEntry);
}

struct DIRENTRY *my_nextfile(struct DIRENTRY *outEntry) {
    return outEntry ? next_entry(outEntry) : NULL;
}

long my_erase(char *path) {
    char p[128];
    map_path(p, path, sizeof(p));
    return f_unlink(p) == FR_OK;
}

long my_format(char *fs) {
    struct DIRENTRY e;
    char dir[128], p[160];
    map_path(dir, fs, sizeof(dir));
    for (struct DIRENTRY *d = my_firstfile(fs, &e); d; d = my_nextfile(d)) {
        snprintf(p, sizeof(p), "%s%s", dir, d->name);
        f_unlink(p);
    }
    return 1;
}

/* psyz.c's default path adjuster calls these; route them to FatFs. */
int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return f_mkdir(path) == FR_OK ? 0 : -1;
}
int remove(const char *path) { return f_unlink(path) == FR_OK ? 0 : -1; }
int rename(const char *from, const char *to) { return f_rename(from, to) == FR_OK ? 0 : -1; }
int stat(const char *path, struct stat *st) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return -1;
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)fi.fsize;
    st->st_mode = (fi.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;
    return 0;
}
