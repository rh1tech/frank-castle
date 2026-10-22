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
#include "pico/mutex.h"
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

static recursive_mutex_t audio_mutex;

int Psyz_AudioInit(void) {
    recursive_mutex_init(&audio_mutex);
    Psyz_SpuInit();
    return 0;
}
void Psyz_AudioLock(void) { recursive_mutex_enter_blocking(&audio_mutex); }
void Psyz_AudioUnlock(void) { recursive_mutex_exit(&audio_mutex); }

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
    unsigned pressed = 0;
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
