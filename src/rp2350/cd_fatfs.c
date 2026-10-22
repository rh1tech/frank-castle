/*
 * frank-castle — FatFs storage backend for cdimage.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A 600+ MB disc image is read at random offsets, so every handle gets a
 * FatFs fast-seek cluster map: without one each f_lseek walks the FAT chain
 * from the start of the file.
 *
 * Reads that land in PSRAM go through an SRAM staging buffer (the SD SPI
 * path must not write PSRAM directly; same rule as frank-fodder).
 */

#include "cd_fatfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HANDLES 4
#define CLMT_WORDS  64   /* enough for a contiguous file; larger maps go to PSRAM */
#define STAGING     4096

extern void *psram_malloc(size_t size);
extern void psram_free(void *ptr);

typedef struct {
    FIL fil;
    DWORD clmt[CLMT_WORDS];
    DWORD *big_clmt; /* PSRAM map for fragmented files */
    bool used;
} handle_t;

static handle_t handles[MAX_HANDLES];
static uint8_t staging[STAGING] __attribute__((aligned(4)));

static inline bool in_sram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x20000000u && a < 0x20082000u;
}

static void *fat_open(const char *path) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        handle_t *h = &handles[i];
        if (h->used) continue;
        FRESULT ofr = f_open(&h->fil, path, FA_READ);
        if (ofr != FR_OK) {
            printf("cd_fatfs: f_open(%s) failed: %d\n", path, ofr);
            return NULL;
        }
        /* Try the small in-handle map first; on FR_NOT_ENOUGH_CORE FatFs
         * leaves the required size in word 0, so allocate exactly that. */
        h->big_clmt = NULL;
        h->clmt[0] = CLMT_WORDS;
        h->fil.cltbl = h->clmt;
        FRESULT fr = f_lseek(&h->fil, CREATE_LINKMAP);
        if (fr == FR_NOT_ENOUGH_CORE) {
            DWORD need = h->clmt[0];
            h->big_clmt = psram_malloc(need * sizeof(DWORD));
            if (h->big_clmt) {
                h->big_clmt[0] = need;
                h->fil.cltbl = h->big_clmt;
                fr = f_lseek(&h->fil, CREATE_LINKMAP);
                printf("cd_fatfs: %s is in %lu fragments\n", path, (unsigned long)(need - 1) / 2);
            }
        }
        if (fr == FR_DISK_ERR) {
            /* the card stopped answering: let the caller re-mount and retry */
            printf("cd_fatfs: %s: disk error while mapping clusters\n", path);
            f_close(&h->fil);
            if (h->big_clmt) psram_free(h->big_clmt);
            h->big_clmt = NULL;
            return NULL;
        }
        if (fr != FR_OK) {
            printf("cd_fatfs: no fast-seek map for %s (%d)\n", path, fr);
            h->fil.cltbl = NULL;
        }
        h->used = true;
        return h;
    }
    return NULL;
}

static uint32_t fat_read(void *handle, uint64_t offset, void *buf, uint32_t len) {
    handle_t *h = handle;
    if (f_lseek(&h->fil, (FSIZE_t)offset) != FR_OK) return 0;
    if (in_sram(buf)) {
        UINT got = 0;
        FRESULT rfr = f_read(&h->fil, buf, len, &got);
        if (rfr != FR_OK) printf("cd_fatfs: f_read failed: %d\n", rfr);
        return got;
    }
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        UINT want = len - done > STAGING ? STAGING : len - done, got = 0;
        if (f_read(&h->fil, staging, want, &got) != FR_OK || got == 0) break;
        memcpy(out + done, staging, got);
        done += got;
        if (got < want) break;
    }
    return done;
}

static uint64_t fat_size(void *handle) {
    return f_size(&((handle_t *)handle)->fil);
}

static void fat_close(void *handle) {
    handle_t *h = handle;
    f_close(&h->fil);
    if (h->big_clmt) psram_free(h->big_clmt);
    h->big_clmt = NULL;
    h->used = false;
}

static void *fat_alloc(size_t size) {
    return psram_malloc(size);
}

const cd_backend_t cd_fatfs_backend = {
    .open = fat_open,
    .read = fat_read,
    .size = fat_size,
    .close = fat_close,
    .alloc = fat_alloc,
};
