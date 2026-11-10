/*
 * frank-castle — psyz audio backend: SPU mixing, HDMI or I2S output.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The game core runs the SPU emulator once per VSync (Audio_FrameFill) and
 * pushes 44.1 kHz stereo frames into a ring. One output drains it (the
 * settings choose which, at run time): the HSTX IRQ on core 1, four frames
 * for each HDMI audio packet (drivers/hdmi/hstx_data_island_queue.c), or
 * the I2S DMA IRQ on core 0 (sharing core 1 with the HSTX IRQ breaks the
 * HDMI timing). Core 1 is otherwise left to the scanout and the rasterizer.
 *
 * Psyz_SpuPullSamples() also advances the root counters, whose handlers are
 * the game's sound driver and must run on the game thread. Psyz_RcntAdd is
 * link-wrapped: calls made on core 1 only accumulate the tick count, and
 * Audio_DrainRcnt() replays them on core 0 at VSync.
 */

#include "audio.h"
#include "board_config.h"
#include "castle_audio.h"
#include "hstx_data_island_queue.h"

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
volatile uint32_t audio_mix_us[2], audio_mix_frames[2]; /* mixing time and frames by core (perf log) */
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

/* ---- Outputs ---- */

/* The settings menu: volume (gain/256), a pause, the output. Paused, the
 * ring is not drained: it stays full, nothing is mixed and the sound
 * driver's ticks stand still, so the music resumes where it stopped. */
static volatile uint32_t audio_gain = 256;
static volatile bool audio_paused;
static volatile int audio_out = AUDIO_OUT_HDMI;

void Audio_SetGain(uint32_t gain) { audio_gain = gain > 256 ? 256 : gain; }
void Audio_Pause(bool on) { audio_paused = on; }

static void __not_in_flash_func(hdmi_source)(uint32_t frames[4]);

/* Only one output drains the ring; the other plays silence. */
void Audio_SetOutput(int out) {
    out = out == AUDIO_OUT_I2S ? AUDIO_OUT_I2S : AUDIO_OUT_HDMI;
    if (out == audio_out) return;
    if (out == AUDIO_OUT_I2S) hstx_di_queue_set_audio_source(NULL); /* the HDMI IRQ stops reading */
    __dmb();
    audio_out = out;
    __dmb();
    if (out == AUDIO_OUT_HDMI) hstx_di_queue_set_audio_source(hdmi_source);
    printf("audio: output %s\n", out == AUDIO_OUT_HDMI ? "HDMI" : "I2S");
}

int Audio_GetOutput(void) { return audio_out; }

static inline uint32_t __not_in_flash_func(scale)(uint32_t v, int32_t gain) {
    int32_t l = ((int32_t)(int16_t)v * gain) >> 8;
    int32_t r = ((int32_t)(int16_t)(v >> 16) * gain) >> 8;
    return (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
}

/* HDMI: the HSTX IRQ (core 1) takes the next audio packet's four frames.
 * It runs as fast as the HDMI clock: 44.1 kHz, from the same crystal. */
volatile uint32_t audio_hdmi_underruns;
static void __not_in_flash_func(hdmi_source)(uint32_t frames[4]) {
    if (audio_paused || audio_out != AUDIO_OUT_HDMI) {
        frames[0] = frames[1] = frames[2] = frames[3] = 0;
        return;
    }
    const uint32_t rd = ring_rd, avail = ring_wr - rd;
    const uint32_t n = avail < 4 ? avail : 4;
    const int32_t gain = (int32_t)audio_gain;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = ring[(rd + i) & (RING_FRAMES - 1)];
        frames[i] = gain >= 256 ? v : scale(v, gain);
    }
    for (uint32_t i = n; i < 4; i++) frames[i] = 0;
    if (n < 4 && audio_running) audio_hdmi_underruns++;
    __dmb();
    ring_rd = rd + n;
}

static void __not_in_flash_func(i2s_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    if (audio_paused || audio_out != AUDIO_OUT_I2S) {
        memset(buf, 0, frames * 4);
        return;
    }
    uint32_t rd = ring_rd, avail = ring_wr - rd;
    uint32_t n = avail < frames ? avail : frames;
    const int32_t gain = (int32_t)audio_gain;
    if (gain >= 256) {
        for (uint32_t i = 0; i < n; i++) buf[i] = ring[(rd + i) & (RING_FRAMES - 1)];
    } else {
        for (uint32_t i = 0; i < n; i++) buf[i] = scale(ring[(rd + i) & (RING_FRAMES - 1)], gain);
    }
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
    if (audio_out == AUDIO_OUT_HDMI) hstx_di_queue_set_audio_source(hdmi_source);
}

/* Game core. Mixing is spread so it never sits on the frame's critical
 * path: DrawSync mixes in small steps for as long as core 1 is still
 * drawing (time the game would only spend waiting), and VSync tops up only
 * when the ring runs low. The root counter handlers (the sound driver) fire
 * on the game thread, as they should. */
#define AUDIO_HIGH 3584 /* ~81 ms: DrawSync fills up to here while waiting */
#define AUDIO_LOW 1536  /* ~35 ms: below this VSync tops up */

/* Mix up to maxn (<= 256) frames if the ring holds fewer than target.
 * Returns true if it mixed. */
static bool fill_step_n(uint32_t target, uint32_t maxn) {
    static short buf[256 * 2];
    extern volatile bool psyz_audio_ready;
    if (!psyz_audio_ready) return false;
    uint32_t level = ring_wr - ring_rd;
    if (level >= target) return false;
    uint32_t n = target - level;
    if (n > maxn) n = maxn;
    uint32_t t0 = time_us_32();
    /* the lock also covers the ring write: core 1 mixes too (Audio_Core1Poll) */
    Psyz_AudioLock();
    level = ring_wr - ring_rd;
    if (level >= target) {
        Psyz_AudioUnlock();
        return false;
    }
    if (n > target - level) n = target - level;
    Psyz_SpuPullSamples(buf, (int)n);
    audio_frame_us += time_us_32() - t0;
    audio_mix_us[0] += time_us_32() - t0;
    audio_mix_frames[0] += n;
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
    Psyz_AudioUnlock();
    audio_running = true;
    return true;
}

bool Audio_FillStep(uint32_t target) { return fill_step_n(target, 256); }

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

/* DrawSync, before core 0 helps rasterize: about a frame of audio over the
 * VSync threshold, so the frame never needs VSync's emergency top-up. */
bool Audio_PreHelpFill(void) { return Audio_FillStep(AUDIO_LOW + 1024); }

/* While helping, when core 0 has nothing to draw: a short step (~0.25 ms)
 * towards AUDIO_HIGH, so it is back for the next command soon. */
bool Audio_HelpIdleFill(void) { return fill_step_n(AUDIO_HIGH, 64); }

/* Core 1 with nothing to draw: one chunk towards AUDIO_HIGH, so core 0
 * seldom has to mix before it helps rasterize. Music it needs comes from
 * the read-ahead only (Psyz_CdReadAllowed). */
bool __not_in_flash_func(Audio_Core1IdlePoll)(void) {
    if (ring_wr - ring_rd >= AUDIO_HIGH) return false;
    extern bool Audio_Core1Poll(void);
    return Audio_Core1Poll();
}

/* Mix one chunk if the ring has room. Returns true if it did work. */
bool __not_in_flash_func(Audio_Core1Poll)(void) {
    static short tmp[CHUNK * 2];
    extern volatile bool psyz_audio_ready;
    extern bool Psyz_AudioTryLock(void);
    if (!psyz_audio_ready || RING_FRAMES - (ring_wr - ring_rd) < CHUNK) return false;
    if (!Psyz_AudioTryLock()) return false; /* game thread is in the SPU */
    uint32_t t0 = time_us_32();
    if (RING_FRAMES - (ring_wr - ring_rd) < CHUNK) { /* core 0 filled it meanwhile */
        Psyz_AudioUnlock();
        return false;
    }
    Psyz_SpuPullSamples(tmp, CHUNK);
    uint32_t dt = time_us_32() - t0;
    if (dt > audio_chunk_max_us) audio_chunk_max_us = dt;
    audio_mix_us[1] += dt;
    audio_mix_frames[1] += CHUNK;
    uint32_t wr = ring_wr;
    for (int i = 0; i < CHUNK; i++)
        ring[(wr + i) & (RING_FRAMES - 1)] = (uint16_t)tmp[2 * i] | ((uint32_t)(uint16_t)tmp[2 * i + 1] << 16);
    __dmb(); /* frames visible to the I2S IRQ on core 0 before the index */
    ring_wr = wr + CHUNK;
    Psyz_AudioUnlock();
    audio_running = true;
    return true;
}
