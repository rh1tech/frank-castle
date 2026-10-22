/*
 * frank-castle — PC-sampling profiler for core 0 (build with -DCASTLE_PROFILE=ON).
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A TIMER1 alarm interrupts core 0 at 4 kHz; the naked handler takes the PC
 * from the exception frame. After PROFILE_DELAY_S seconds it samples for
 * PROFILE_SAMPLES ticks, then the game thread prints them on the UART
 * ("[prof] <hex pc>"). tools/profile.py turns them into a per-function table.
 */
#include "hardware/irq.h"
#include "hardware/structs/timer.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define PROFILE_HZ 4000
#define PROFILE_SAMPLES 12000
#define PROFILE_DELAY_S 14

static uint32_t samples[PROFILE_SAMPLES] __attribute__((section(".psram_bss.prof")));
static volatile uint32_t nsamples;
static volatile bool done, printed;
static uint64_t start_us;

/* return addresses of samples that landed in memcpy (who is copying?) */
static uint32_t memcpy_lr[256];
static volatile uint32_t n_memcpy_lr;
extern char memcpy[];

void __not_in_flash_func(profiler_record)(uint32_t pc, uint32_t lr) {
    timer_hw_t *t = timer1_hw;
    t->intr = 1u << 0;
    t->alarm[0] = t->timerawl + 1000000 / PROFILE_HZ;
    if (done) return;
    if (time_us_64() < start_us) return;
    samples[nsamples++] = pc;
    if (pc - (uint32_t)(uintptr_t)memcpy < 0xf0 && n_memcpy_lr < 256) memcpy_lr[n_memcpy_lr++] = lr;
    if (nsamples >= PROFILE_SAMPLES) {
        done = true;
        t->inte &= ~1u;
    }
}

/* exception frame: r0 r1 r2 r3 r12 lr pc xpsr; thread mode uses MSP */
void __attribute__((naked)) __not_in_flash_func(profiler_isr)(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "ldr r1, [r0, #20]\n"
        "ldr r0, [r0, #24]\n"
        "b profiler_record\n");
}

void profiler_start(void) {
    start_us = time_us_64() + PROFILE_DELAY_S * 1000000ull;
    timer_hw_t *t = timer1_hw;
    irq_set_exclusive_handler(TIMER1_IRQ_0, profiler_isr);
    irq_set_priority(TIMER1_IRQ_0, 0x40);
    t->inte |= 1u;
    t->alarm[0] = t->timerawl + 1000;
    irq_set_enabled(TIMER1_IRQ_0, true);
}

/* Start sampling now instead of after PROFILE_DELAY_S (first call wins). */
void profiler_trigger(void) {
    static bool triggered;
    if (triggered) return;
    triggered = true;
    start_us = time_us_64();
}

/* called from the game thread (VSync) */
void profiler_poll(void) {
    if (!done || printed) return;
    printed = true;
    printf("[prof] begin %lu\n", (unsigned long)nsamples);
    for (uint32_t i = 0; i < nsamples; i++) printf("[prof] %08lx\n", (unsigned long)samples[i]);
    for (uint32_t i = 0; i < n_memcpy_lr; i++) printf("[proflr] %08lx\n", (unsigned long)memcpy_lr[i]);
    printf("[prof] end\n");
}
