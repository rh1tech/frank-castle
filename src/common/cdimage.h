/*
 * frank-castle — CUE/BIN disc image reader with an ISO9660 file index.
 *
 * Portable C: the storage backend is a small vtable, so the same code runs
 * on the RP2350 (FatFs) and in the host PC build (stdio).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef CDIMAGE_H
#define CDIMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CD_SECTOR_RAW  2352
#define CD_SECTOR_DATA 2048

typedef struct {
    /* Open a file by path; returns an opaque handle or NULL. */
    void *(*open)(const char *path);
    /* Absolute-offset read; returns bytes read. */
    uint32_t (*read)(void *handle, uint64_t offset, void *buf, uint32_t len);
    uint64_t (*size)(void *handle);
    void (*close)(void *handle);
    /* Allocate the file index (may be slow memory, e.g. PSRAM). */
    void *(*alloc)(size_t size);
} cd_backend_t;

typedef struct {
    char path[56];     /* upper case, '/' separated, no ";1": "ST/WRP/F_WRP.BIN" */
    uint32_t lba;
    uint32_t size;
} cd_entry_t;

typedef struct {
    const cd_entry_t *entry;
    uint32_t pos;
} cd_file_t;

typedef enum {
    CD_OK = 0,
    CD_ERR_CUE,       /* cue missing or unparsable */
    CD_ERR_BIN,       /* track file missing */
    CD_ERR_FORMAT,    /* unsupported track mode */
    CD_ERR_ISO,       /* no ISO9660 volume descriptor */
    CD_ERR_NOMEM,
} cd_result_t;

/* Open a .cue (or a bare .bin/.iso). The directory tree is indexed once. */
cd_result_t cd_open(const cd_backend_t *backend, const char *path);
void cd_close(void);
bool cd_is_open(void);

/* Volume info */
const char *cd_volume_id(void);
uint32_t cd_entry_count(void);
const cd_entry_t *cd_entry_at(uint32_t i);

/* Lookup is case-insensitive and accepts '/' or '\\', a leading
 * separator, "cdrom:" and a ";1" version suffix. */
const cd_entry_t *cd_lookup(const char *path);

/* Sector access. Data reads return the 2048-byte user area of Mode1 /
 * Mode2 Form1 sectors; raw reads return the full 2352-byte sector (XA).
 * Both return the number of sectors read. */
uint32_t cd_read_data(uint32_t lba, uint32_t count, void *dst);
uint32_t cd_read_raw(uint32_t lba, uint32_t count, void *dst);
bool cd_has_raw_sectors(void);

/* Byte-stream access to a file in the image. */
bool cd_fopen(cd_file_t *f, const char *path);
uint32_t cd_fread(cd_file_t *f, void *dst, uint32_t len);
void cd_fseek(cd_file_t *f, uint32_t pos);

#endif /* CDIMAGE_H */
