/*
 * frank-castle — CUE/BIN disc image reader with an ISO9660 file index.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "cdimage.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define STAGE_SECTORS 4

static struct {
    const cd_backend_t *be;
    void *bin;
    uint32_t sector_size;  /* 2352 or 2048 */
    uint32_t data_offset;  /* 24 (Mode2 Form1), 16 (Mode1) or 0 (cooked) */
    uint32_t sectors;
    char volume_id[33];
    cd_entry_t *entries;
    uint32_t count, capacity;
    /* single-sector cache for unaligned file reads */
    uint32_t cache_lba;
    uint8_t cache[CD_SECTOR_DATA];
} cd;

static uint8_t stage[STAGE_SECTORS * CD_SECTOR_RAW];

bool cd_is_open(void) { return cd.bin != NULL; }
const char *cd_volume_id(void) { return cd.volume_id; }
uint32_t cd_entry_count(void) { return cd.count; }
const cd_entry_t *cd_entry_at(uint32_t i) { return i < cd.count ? &cd.entries[i] : NULL; }
bool cd_has_raw_sectors(void) { return cd.sector_size == CD_SECTOR_RAW; }

/* ------------------------------------------------------------------ */
/* Sector access                                                       */
/* ------------------------------------------------------------------ */

uint32_t cd_read_raw(uint32_t lba, uint32_t count, void *dst) {
    if (!cd.bin || cd.sector_size != CD_SECTOR_RAW) return 0;
    if (lba >= cd.sectors) return 0;
    if (lba + count > cd.sectors) count = cd.sectors - lba;
    uint32_t got = cd.be->read(cd.bin, (uint64_t)lba * CD_SECTOR_RAW, dst, count * CD_SECTOR_RAW);
    return got / CD_SECTOR_RAW;
}

uint32_t cd_read_data(uint32_t lba, uint32_t count, void *dst) {
    if (!cd.bin || lba >= cd.sectors) return 0;
    if (lba + count > cd.sectors) count = cd.sectors - lba;
    if (cd.sector_size == CD_SECTOR_DATA) {
        uint32_t got = cd.be->read(cd.bin, (uint64_t)lba * CD_SECTOR_DATA, dst, count * CD_SECTOR_DATA);
        return got / CD_SECTOR_DATA;
    }
    uint8_t *out = dst;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > STAGE_SECTORS) n = STAGE_SECTORS;
        uint32_t got = cd_read_raw(lba + done, n, stage);
        for (uint32_t i = 0; i < got; i++)
            memcpy(out + (done + i) * CD_SECTOR_DATA, stage + i * CD_SECTOR_RAW + cd.data_offset,
                   CD_SECTOR_DATA);
        done += got;
        if (got < n) break;
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* ISO9660 index                                                       */
/* ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef void (*walk_fn)(const char *path, uint32_t lba, uint32_t size);

static void index_count(const char *path, uint32_t lba, uint32_t size) {
    (void)path; (void)lba; (void)size;
    cd.count++;
}

static void index_store(const char *path, uint32_t lba, uint32_t size) {
    if (cd.count >= cd.capacity) return;
    cd_entry_t *e = &cd.entries[cd.count++];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = 0;
    e->lba = lba;
    e->size = size;
}

static int walk_dir(uint32_t lba, uint32_t size, const char *prefix, int depth, walk_fn fn) {
    if (depth > 8) return 0;
    uint8_t sec[CD_SECTOR_DATA];
    uint32_t nsec = (size + CD_SECTOR_DATA - 1) / CD_SECTOR_DATA;
    for (uint32_t s = 0; s < nsec; s++) {
        if (cd_read_data(lba + s, 1, sec) != 1) return -1;
        uint32_t off = 0;
        while (off < CD_SECTOR_DATA) {
            const uint8_t *r = sec + off;
            uint8_t len = r[0];
            if (len == 0) break; /* rest of this sector is padding */
            uint32_t ext = le32(r + 2), sz = le32(r + 10);
            uint8_t flags = r[25], nlen = r[32];
            const char *name = (const char *)r + 33;
            off += len;
            if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue; /* . and .. */

            char path[64];
            int n = snprintf(path, sizeof(path), "%s%s%.*s", prefix, *prefix ? "/" : "", nlen, name);
            if (n <= 0 || n >= (int)sizeof(path)) continue;
            char *semi = strchr(path, ';');
            if (semi) *semi = 0;
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '.') path[pl - 1] = 0; /* "NAME." with no extension */
            for (char *p = path; *p; p++) *p = (char)toupper((unsigned char)*p);

            if (flags & 2) {
                if (walk_dir(ext, sz, path, depth + 1, fn) < 0) return -1;
            } else {
                fn(path, ext, sz);
            }
        }
    }
    return 0;
}

static cd_result_t build_index(void) {
    uint8_t pvd[CD_SECTOR_DATA];
    if (cd_read_data(16, 1, pvd) != 1) return CD_ERR_ISO;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return CD_ERR_ISO;

    memcpy(cd.volume_id, pvd + 40, 32);
    cd.volume_id[32] = 0;
    for (int i = 31; i >= 0 && cd.volume_id[i] == ' '; i--) cd.volume_id[i] = 0;

    const uint8_t *root = pvd + 156;
    uint32_t root_lba = le32(root + 2), root_size = le32(root + 10);

    cd.count = 0;
    if (walk_dir(root_lba, root_size, "", 0, index_count) < 0) return CD_ERR_ISO;
    cd.capacity = cd.count;
    cd.entries = cd.be->alloc(sizeof(cd_entry_t) * (cd.capacity ? cd.capacity : 1));
    if (!cd.entries) return CD_ERR_NOMEM;
    cd.count = 0;
    if (walk_dir(root_lba, root_size, "", 0, index_store) < 0) return CD_ERR_ISO;
    return CD_OK;
}

/* ------------------------------------------------------------------ */
/* CUE parsing                                                         */
/* ------------------------------------------------------------------ */

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

/* Detect raw vs cooked and the user-data offset from sector 16's header. */
static cd_result_t probe_layout(uint64_t file_size, int cue_raw) {
    if (cue_raw == 0 || (cue_raw < 0 && file_size % CD_SECTOR_RAW != 0)) {
        cd.sector_size = CD_SECTOR_DATA;
        cd.data_offset = 0;
        cd.sectors = (uint32_t)(file_size / CD_SECTOR_DATA);
        return CD_OK;
    }
    cd.sector_size = CD_SECTOR_RAW;
    cd.sectors = (uint32_t)(file_size / CD_SECTOR_RAW);
    uint8_t hdr[16];
    if (cd.be->read(cd.bin, 16ull * CD_SECTOR_RAW, hdr, 16) != 16) return CD_ERR_FORMAT;
    static const uint8_t sync[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    if (memcmp(hdr, sync, 12) != 0) return CD_ERR_FORMAT;
    if (hdr[15] == 1)
        cd.data_offset = 16;
    else if (hdr[15] == 2)
        cd.data_offset = 24;
    else
        return CD_ERR_FORMAT;
    return CD_OK;
}

cd_result_t cd_open(const cd_backend_t *backend, const char *path) {
    cd_close();
    memset(&cd, 0, sizeof(cd));
    cd.be = backend;
    cd.cache_lba = UINT32_MAX;

    char bin_path[256];
    int cue_raw = -1; /* -1 unknown, 0 cooked, 1 raw */

    if (ends_with_ci(path, ".cue")) {
        void *h = backend->open(path);
        if (!h) return CD_ERR_CUE;
        char text[2048];
        uint32_t n = backend->read(h, 0, text, sizeof(text) - 1);
        backend->close(h);
        text[n] = 0;

        /* FILE "name" BINARY */
        char *f = strstr(text, "FILE");
        if (!f) return CD_ERR_CUE;
        f += 4;
        while (*f == ' ' || *f == '\t') f++;
        char name[128];
        size_t len = 0;
        if (*f == '"') {
            f++;
            while (*f && *f != '"' && len < sizeof(name) - 1) name[len++] = *f++;
        } else {
            while (*f && *f != ' ' && *f != '\t' && len < sizeof(name) - 1) name[len++] = *f++;
        }
        name[len] = 0;

        /* First TRACK sets the layout */
        char *t = strstr(text, "TRACK");
        if (t) {
            if (strstr(t, "/2352"))
                cue_raw = 1;
            else if (strstr(t, "MODE1/2048"))
                cue_raw = 0;
            else
                return CD_ERR_FORMAT;
        }

        /* bin path is relative to the cue's directory */
        const char *slash = strrchr(path, '/');
        size_t dirlen = slash ? (size_t)(slash - path + 1) : 0;
        if (dirlen + strlen(name) + 1 > sizeof(bin_path)) return CD_ERR_CUE;
        memcpy(bin_path, path, dirlen);
        strcpy(bin_path + dirlen, name);
    } else {
        strncpy(bin_path, path, sizeof(bin_path) - 1);
        bin_path[sizeof(bin_path) - 1] = 0;
    }

    cd.bin = backend->open(bin_path);
    if (!cd.bin) return CD_ERR_BIN;

    cd_result_t r = probe_layout(backend->size(cd.bin), cue_raw);
    if (r == CD_OK) r = build_index();
    if (r != CD_OK) cd_close();
    return r;
}

void cd_close(void) {
    if (cd.bin && cd.be) cd.be->close(cd.bin);
    cd.bin = NULL;
    /* the index lives in the backend's allocator and is reclaimed with it */
    cd.entries = NULL;
    cd.count = 0;
}

/* ------------------------------------------------------------------ */
/* Lookup and file streams                                             */
/* ------------------------------------------------------------------ */

static void normalise(const char *in, char *out, size_t outlen) {
    if (strncmp(in, "cdrom:", 6) == 0 || strncmp(in, "CDROM:", 6) == 0) in += 6;
    while (*in == '/' || *in == '\\') in++;
    size_t n = 0;
    for (; *in && *in != ';' && n < outlen - 1; in++)
        out[n++] = (*in == '\\') ? '/' : (char)toupper((unsigned char)*in);
    out[n] = 0;
}

const cd_entry_t *cd_lookup(const char *path) {
    char key[64];
    normalise(path, key, sizeof(key));
    for (uint32_t i = 0; i < cd.count; i++)
        if (strcmp(cd.entries[i].path, key) == 0) return &cd.entries[i];
    return NULL;
}

bool cd_fopen(cd_file_t *f, const char *path) {
    f->entry = cd_lookup(path);
    f->pos = 0;
    return f->entry != NULL;
}

void cd_fseek(cd_file_t *f, uint32_t pos) {
    f->pos = pos > f->entry->size ? f->entry->size : pos;
}

uint32_t cd_fread(cd_file_t *f, void *dst, uint32_t len) {
    if (!f->entry) return 0;
    uint32_t left = f->entry->size - f->pos;
    if (len > left) len = left;
    uint8_t *out = dst;
    uint32_t done = 0;
    while (done < len) {
        uint32_t lba = f->entry->lba + f->pos / CD_SECTOR_DATA;
        uint32_t in_sec = f->pos % CD_SECTOR_DATA;
        uint32_t want = len - done;
        if (in_sec == 0 && want >= CD_SECTOR_DATA) {
            /* whole sectors straight into the destination */
            uint32_t n = want / CD_SECTOR_DATA;
            uint32_t got = cd_read_data(lba, n, out + done);
            if (got == 0) break;
            done += got * CD_SECTOR_DATA;
            f->pos += got * CD_SECTOR_DATA;
            continue;
        }
        if (cd.cache_lba != lba) {
            if (cd_read_data(lba, 1, cd.cache) != 1) break;
            cd.cache_lba = lba;
        }
        uint32_t n = CD_SECTOR_DATA - in_sec;
        if (n > want) n = want;
        memcpy(out + done, cd.cache + in_sec, n);
        done += n;
        f->pos += n;
    }
    return done;
}
