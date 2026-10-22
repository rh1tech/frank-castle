/*
 * frank-castle — psyz audio backend: SPU mixing on core 1, I2S output.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The game core runs the SPU emulator once per VSync (Audio_FrameFill) and
 * pushes 44.1 kHz stereo frames into a ring; the I2S DMA IRQ (also core 0:
 * sharing core 1 with the HSTX IRQ breaks the HDMI timing) only copies from
 * the ring. Core 1 is left to the scanout and the rasterizer.
 *
 * Psyz_SpuPullSamples() also advances the root counters, whose handlers are
 * the game's sound driver and must run on the game thread. Psyz_RcntAdd is
 * link-wrapped: calls made on core 1 only accumulate the tick count, and
 * Audio_DrainRcnt() replays them on core 0 at VSync.
 */

#include "audio.h"
#include "board_config.h"

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

extern void Psyz_AudioLock(void);
extern void Psyz_AudioUnlock(void);
extern void Psyz_SpuPullSamples(short *out, int num_frames);

#define SAMPLE_RATE 44100
#define RING_FRAMES 4096 /* ~93 ms */
#define CHUNK 8          /* frames mixed per step: short, so scanout copies never wait long */
#define DMA_FRAMES 441   /* 10 ms per I2S DMA buffer */

static uint32_t ring[RING_FRAMES]; /* packed L|R<<16 */
static volatile uint32_t ring_wr, ring_rd;
volatile uint32_t audio_underruns;
volatile uint32_t audio_chunk_max_us; /* worst mixing step (diagnostic) */
volatile uint32_t audio_peak;         /* loudest sample mixed since the last perf log */
volatile uint32_t audio_frame_us;     /* mixing time this frame (reset at VSync) */
static bool audio_running;

/* ---- root counter ticks from core 1, replayed on core 0 ---- */

extern void __real_Psyz_RcntAdd(int n);
static volatile int32_t pending_rcnt;

void __wrap_Psyz_RcntAdd(int n) {
    if (get_core_num() == 1) {
        __atomic_fetch_add(&pending_rcnt, n, __ATOMIC_RELAXED);
    } else {
        __real_Psyz_RcntAdd(n);
    }
}

void Audio_DrainRcnt(void) {
    int32_t n = pending_rcnt;
    if (!n) return;
    __atomic_fetch_sub(&pending_rcnt, n, __ATOMIC_SEQ_CST);
    __real_Psyz_RcntAdd(n);
}

/* ---- I2S ---- */

static void __not_in_flash_func(i2s_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    uint32_t rd = ring_rd, avail = ring_wr - rd;
    uint32_t n = avail < frames ? avail : frames;
    for (uint32_t i = 0; i < n; i++) buf[i] = ring[(rd + i) & (RING_FRAMES - 1)];
    if (n < frames) {
        memset(buf + n, 0, (frames - n) * 4);
        if (audio_running) audio_underruns++;
    }
    ring_rd = rd + n;
}

void Audio_Init(void) {
    static i2s_config_t cfg;
    cfg.sample_freq = SAMPLE_RATE;
    cfg.channel_count = 2;
    cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
    cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    cfg.pio = pio0;
    cfg.dma_trans_count = DMA_FRAMES;
    cfg.volume = 0;
    i2s_init(&cfg);
    i2s_set_fill_callback(i2s_fill);
    i2s_start();
}

/* Game core. Mixing is spread so it never sits on the frame's critical
 * path: DrawSync mixes in small steps for as long as core 1 is still
 * drawing (time the game would only spend waiting), and VSync tops up only
 * when the ring runs low. The root counter handlers (the sound driver) fire
 * on the game thread, as they should. */
#define AUDIO_HIGH 3584 /* ~81 ms: DrawSync fills up to here while waiting */
#define AUDIO_LOW 1536  /* ~35 ms: below this VSync tops up */

/* Mix up to 256 frames if the ring holds fewer than target. Returns true if
 * it mixed. */
bool Audio_FillStep(uint32_t target) {
    static short buf[256 * 2];
    extern volatile bool psyz_audio_ready;
    if (!psyz_audio_ready) return false;
    uint32_t level = ring_wr - ring_rd;
    if (level >= target) return false;
    uint32_t n = target - level;
    if (n > 256) n = 256;
    uint32_t t0 = time_us_32();
    Psyz_AudioLock();
    Psyz_SpuPullSamples(buf, (int)n);
    Psyz_AudioUnlock();
    audio_frame_us += time_us_32() - t0;
    uint32_t wr = ring_wr;
    uint32_t peak = audio_peak;
    for (uint32_t i = 0; i < n; i++) {
        int l = buf[2 * i], r = buf[2 * i + 1];
        uint32_t m = (uint32_t)(l < 0 ? -l : l) | (uint32_t)(r < 0 ? -r : r);
        if (m > peak) peak = m;
        ring[(wr + i) & (RING_FRAMES - 1)] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
    }
    audio_peak = peak;
    __dmb();
    ring_wr = wr + n;
    audio_running = true;
    return true;
}

uint32_t Audio_Level(void) { return ring_wr - ring_rd; }

/* VSync: emergency top-up only (the DrawSync wait normally keeps it full). */
void Audio_FrameFill(void) {
    if (ring_wr - ring_rd >= AUDIO_LOW) return;
    extern void Psyz_CdPrefetch(void);
    Psyz_CdPrefetch(); /* read music sectors first: mixing must not hit the card */
    while (Audio_FillStep(AUDIO_LOW + 512)) {
    }
}

/* DrawSync, while core 1 is still busy: one step towards AUDIO_HIGH. */
bool Audio_IdleFill(void) { return Audio_FillStep(AUDIO_HIGH); }

/* Mix one chunk if the ring has room. Returns true if it did work. */
bool __not_in_flash_func(Audio_Core1Poll)(void) {
    static short tmp[CHUNK * 2];
    extern volatile bool psyz_audio_ready;
    extern bool Psyz_AudioTryLock(void);
    if (!psyz_audio_ready || RING_FRAMES - (ring_wr - ring_rd) < CHUNK) return false;
    if (!Psyz_AudioTryLock()) return false; /* game thread is in the SPU */
    uint32_t t0 = time_us_32();
    Psyz_SpuPullSamples(tmp, CHUNK);
    Psyz_AudioUnlock();
    uint32_t dt = time_us_32() - t0;
    if (dt > audio_chunk_max_us) audio_chunk_max_us = dt;
    uint32_t wr = ring_wr;
    for (int i = 0; i < CHUNK; i++)
        ring[(wr + i) & (RING_FRAMES - 1)] = (uint16_t)tmp[2 * i] | ((uint32_t)(uint16_t)tmp[2 * i + 1] << 16);
    __dmb(); /* frames visible to the I2S IRQ on core 0 before the index */
    ring_wr = wr + CHUNK;
    audio_running = true;
    return true;
}
