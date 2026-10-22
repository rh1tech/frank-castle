/*
 * frank-castle — non-blocking stdio on the debug UART.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The SDK's UART stdio blocks until every byte is on the wire: a 1 KB perf
 * report at 115200 baud stalls the game for ~90 ms, five or six frames.
 * Output goes into a ring instead, drained by a 1 ms timer interrupt into the
 * UART's 32-byte FIFO. When the ring is full, or interrupts are off (crash
 * handlers), writes fall back to blocking so nothing is lost.
 */

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_uart.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define RING 4096 /* power of two */

static char ring[RING];
static volatile uint32_t wr, rd;
static repeating_timer_t drain_timer;
static uart_inst_t *const u = uart0;

static void drain(void) {
    while (rd != wr && uart_is_writable(u)) {
        uart_get_hw(u)->dr = (uint8_t)ring[rd & (RING - 1)];
        rd = rd + 1;
    }
}

static bool drain_cb(repeating_timer_t *t) {
    (void)t;
    drain();
    return true;
}

static bool irqs_enabled(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    return !(primask & 1);
}

static void put(char c) {
    while (wr - rd >= RING) { /* full: make room ourselves */
        uint32_t s = save_and_disable_interrupts();
        drain();
        restore_interrupts(s);
    }
    ring[wr & (RING - 1)] = c;
    __dmb();
    wr = wr + 1;
}

static void out_chars(const char *buf, int len) {
    if (!irqs_enabled()) {
        /* fault handler or critical section: nobody will drain, go direct */
        while (rd != wr) {
            while (!uart_is_writable(u)) tight_loop_contents();
            uart_get_hw(u)->dr = (uint8_t)ring[rd & (RING - 1)];
            rd = rd + 1;
        }
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') uart_putc_raw(u, '\r');
            uart_putc_raw(u, buf[i]);
        }
        return;
    }
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') put('\r');
        put(buf[i]);
    }
}

static void out_flush(void) {
    while (rd != wr) {
        uint32_t s = save_and_disable_interrupts();
        drain();
        restore_interrupts(s);
    }
    uart_tx_wait_blocking(u);
}

static int in_chars(char *buf, int len) {
    int n = 0;
    while (n < len && uart_is_readable(u)) buf[n++] = (char)uart_get_hw(u)->dr;
    return n ? n : PICO_ERROR_NO_DATA;
}

static stdio_driver_t buffered_uart = {
    .out_chars = out_chars,
    .out_flush = out_flush,
    .in_chars = in_chars,
};

/* Call after stdio_init_all() (the UART is set up by then); takes over from
 * the blocking SDK driver. Runs the drain timer on the calling core. */
void uart_buffered_init(void) {
    stdio_set_driver_enabled(&stdio_uart, false);
    stdio_set_driver_enabled(&buffered_uart, true);
    add_repeating_timer_us(-1000, drain_cb, NULL, &drain_timer);
}

void uart_buffered_flush(void) { out_flush(); }
