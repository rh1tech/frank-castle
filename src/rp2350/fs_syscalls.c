/*
 * frank-castle — read-only newlib file syscalls on the SD card.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * psyz's libcd parses the CUE and streams XA/CD-DA from the BIN with plain
 * stdio. These overrides of the SDK's weak stubs route fopen/fread/fseek on
 * fds >= 3 to the cd_fatfs backend, which keeps a FatFs fast-seek map per
 * open file, so seeking around a fragmented 600 MB image stays cheap.
 * Writes are refused: nothing in the game needs them, and an accidental
 * write stream on the SD card is not worth the risk.
 */

#include "cd_fatfs.h"

#include "pico/stdio.h"
#include "pico/time.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FIRST_FD 3
#define MAX_FILES 4

static struct {
    void *h;
    uint64_t pos, size;
} files[MAX_FILES];

static int slot_of(int fd) {
    int i = fd - FIRST_FD;
    return (i >= 0 && i < MAX_FILES && files[i].h) ? i : -1;
}

int _open(const char *fn, int oflag, ...) {
    if ((oflag & O_ACCMODE) != O_RDONLY) {
        printf("fs: refusing to write %s\n", fn);
        errno = EROFS;
        return -1;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].h) continue;
        void *h = cd_fatfs_backend.open(fn);
        if (!h) {
            printf("fs: cannot open %s\n", fn);
            errno = ENOENT;
            return -1;
        }
        files[i].h = h;
        files[i].pos = 0;
        files[i].size = cd_fatfs_backend.size(h);
        return FIRST_FD + i;
    }
    printf("fs: no free fd for %s\n", fn);
    errno = EMFILE;
    return -1;
}

int _close(int fd) {
    int i = slot_of(fd);
    if (i < 0) return -1;
    cd_fatfs_backend.close(files[i].h);
    files[i].h = NULL;
    return 0;
}

int _read(int fd, char *buf, int len) {
    if (fd == 0) return stdio_get_until(buf, len, at_the_end_of_time);
    int i = slot_of(fd);
    if (i < 0) {
        errno = EBADF;
        return -1;
    }
    if (files[i].pos >= files[i].size) return 0;
    uint64_t left = files[i].size - files[i].pos;
    if ((uint64_t)len > left) len = (int)left;
    uint32_t got = cd_fatfs_backend.read(files[i].h, files[i].pos, buf, (uint32_t)len);
    files[i].pos += got;
    return (int)got;
}

int _write(int fd, char *buf, int len) {
    if (fd == 1 || fd == 2) {
        stdio_put_string(buf, len, false, true);
        return len;
    }
    errno = EBADF;
    return -1;
}

off_t _lseek(int fd, off_t off, int whence) {
    int i = slot_of(fd);
    if (i < 0) {
        errno = EBADF;
        return -1;
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)files[i].pos : (int64_t)files[i].size;
    int64_t p = base + off;
    if (p < 0) {
        errno = EINVAL;
        return -1;
    }
    files[i].pos = (uint64_t)p;
    return (off_t)p;
}

int _fstat(int fd, struct stat *st) {
    if (fd < FIRST_FD) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    int i = slot_of(fd);
    if (i < 0) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFREG;
    st->st_size = (off_t)files[i].size;
    st->st_blksize = 512; /* small: sector-sized freads then go straight to FatFs */
    return 0;
}
