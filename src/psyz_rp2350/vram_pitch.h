/*
 * frank-castle — row pitch of the PS1 VRAM copy in PSRAM.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * VRAM is 1024 pixels wide, but its rows are stored VRAM_PITCH pixels apart.
 * With the natural 2048-byte pitch, texture rows four apart share sets in the
 * 16 KB two-way XIP cache, so a sheared or rotated quad walking down a
 * texture evicts its own lines and misses on almost every texel. Four pixels
 * of padding shift each row by one cache line.
 */
#ifndef VRAM_PITCH_H
#define VRAM_PITCH_H

#define VRAM_PITCH 1028

#endif
