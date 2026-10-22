/*
 * frank-castle — Castlevania: Symphony of the Night port for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 *
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

/* Board bring-up shared by the diag and game firmware. */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* stdio, then voltage + flash timings + sys clock. Returns the clock in Hz. */
uint32_t platform_init_clocks(void);

/* PSRAM controller, .psram_data/.psram_bss, dlmalloc heap. Must follow the
 * final clock change. Returns the detected CS pin. */
unsigned platform_init_psram(void);

/* TinyUSB host + 1 kHz tuh_task() pump. */
void platform_init_usb(void);

/* Mount the SD card. Returns the FatFs result (0 = FR_OK). */
int platform_mount_sd(void);

/* Print and clear crash info saved by the previous boot's fault handler. */
void platform_report_previous_crash(void);

/* DWT cycle counter, for profiling. */
void platform_cyccnt_enable(void);
static inline uint32_t platform_cyccnt(void) {
    return *(volatile uint32_t *)0xE0001004;
}

void __attribute__((noreturn)) platform_reboot(bool bootsel);
void platform_uart_cmd(int ch);
void platform_usb_poll(void);

#endif /* PLATFORM_H */
