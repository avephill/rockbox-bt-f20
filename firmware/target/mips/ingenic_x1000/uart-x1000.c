/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Interrupt-driven UART driver for Ingenic X1000
 *
 * Based on x1000_dualboot_init_uart2() in boot-x1000.c by Aidan MacDonald.
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/

#include "uart-x1000.h"
#include "system.h"
#include "kernel.h"
#include "irq-x1000.h"
#include "x1000/uart.h"
#include "x1000/cpm.h"
#include "x1000/intc.h"

/* ---- ring buffer helpers (power-of-2 size assumed) ---- */

typedef struct {
    uint8_t*    buf;
    size_t      mask;   /* bufsz - 1 */
    volatile size_t head; /* written by ISR */
    volatile size_t tail; /* consumed by reader */
} ringbuf_t;

static inline void rb_push(ringbuf_t* r, uint8_t c)
{
    size_t next = (r->head + 1) & r->mask;
    if(next != r->tail) {          /* drop on overflow */
        r->buf[r->head] = c;
        r->head = next;
    }
}

static inline int rb_pop(ringbuf_t* r, uint8_t* out)
{
    if(r->tail == r->head) return 0;
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) & r->mask;
    return 1;
}

/* ---- per-port state ---- */

typedef struct {
    ringbuf_t   rx;
    uart_rx_cb_t rx_cb;
} uart_state_t;

static uart_state_t uart_state[3];

/* ---- baud rate calculation ----
 *
 * The X1000 UART generates baud = exclk / (UMR * div) where UMR is the
 * oversample ratio (4..31) and div is a 16-bit divisor in UDLHR:UDLLR.
 * For standard bauds UMR=16 gives the expected low-error divisor; for
 * high-speed (>1.5M on 24MHz exclk) we drop UMR so div stays >= 1.
 */
static void calc_baud_params(unsigned baud, unsigned exclk_hz,
                             unsigned* out_div, unsigned* out_umr)
{
    unsigned total = (exclk_hz + baud / 2) / baud;   /* total oversample */
    if(total >= 32) {
        /* Standard bauds: keep UMR=16, use div */
        *out_umr = 16;
        *out_div = (total + 8) / 16;
    } else {
        /* High-speed: UMR = total, div = 1 */
        *out_div = 1;
        *out_umr = total < 4 ? 4 : (total > 31 ? 31 : total);
    }
}

/* ---- CPM clock gating and INTC mask ---- */

static void uart_ungate_clock(int port)
{
    switch(port) {
    case 0: jz_writef(CPM_CLKGR, UART0(0)); break;
    case 1: jz_writef(CPM_CLKGR, UART1(0)); break;
    case 2: jz_writef(CPM_CLKGR, UART2(0)); break;
    }
}

static void uart_intc_enable(int port, int enable)
{
    /* INTC group 1 bits: UART2=17, UART1=18, UART0=19 */
    static const int irq1_bits[3] = { IRQ1_UART0, IRQ1_UART1, IRQ1_UART2 };
    if(port < 0 || port > 2) return;
    if(enable)
        jz_clr(INTC_MSK(1), 1 << irq1_bits[port]);
    else
        jz_set(INTC_MSK(1), 1 << irq1_bits[port]);
}

/* ---- public API ---- */

void uart_x1000_init(int port, unsigned baud, unsigned exclk_hz,
                     uart_rx_cb_t rx_cb,
                     uint8_t* rx_buf, size_t rx_bufsz)
{
    uart_state_t* s = &uart_state[port];
    s->rx.buf  = rx_buf;
    s->rx.mask = rx_bufsz - 1;
    s->rx.head = s->rx.tail = 0;
    s->rx_cb   = rx_cb;

    uart_ungate_clock(port);
    uart_intc_enable(port, 0);  /* mask at INTC during init */

    /* Disable interrupts during init */
    jz_write(UART_UIER(port), 0);

    /* FIFO setup: enable, reset both FIFOs, 1-byte RX trigger.
     * RDTR(0)=1byte, RDTR(1)=16byte, RDTR(2)=32byte, RDTR(3)=60byte.
     * HCI packets can be as short as 4 bytes; use 1-byte so DRY asserts
     * on any received character, not just when 16 bytes accumulate. */
    jz_overwritef(UART_UFCR(port),
                  RDTR(0),  /* RX FIFO trigger = 1 byte */
                  UME(0),
                  DME(0),
                  TFRT(1),
                  RFRT(1),
                  FME(1));

    /* IR mode: disabled */
    jz_overwritef(UART_ISR(port),
                  RDPL(1), TDPL(1), XMODE(1),
                  RCVEIR(0), XMITIR(0));

    /* 8N1 */
    jz_overwritef(UART_ULCR(port), DLAB(0),
                  WLS_V(8BITS),
                  SBLS_V(1_STOP_BIT),
                  PARE(0),
                  SBK(0));

    uart_x1000_set_baud(port, baud, exclk_hz);

    /* Enable FIFO with RX-ready interrupt, 1-byte trigger */
    jz_overwritef(UART_UFCR(port),
                  RDTR(0),
                  DME(0),
                  UME(1),
                  TFRT(1),
                  RFRT(1),
                  FME(1));

    /* Enable RX data available interrupt only.
     * TX interrupt is not used — we poll TDRQ in uart_x1000_write(). */
    if(rx_cb) {
        jz_write(UART_UIER(port), BM_UART_UIER_RDRIE);
        uart_intc_enable(port, 1);
    } else {
        jz_write(UART_UIER(port), 0);
    }
}

void uart_x1000_modem_bt_host_ready(int port)
{
    if(port < 0 || port > 2)
        return;
    /* Many combo BT chips only drive UART TX toward the host when CTS (seen
     * from the module) allows it — often wired to host RTS. With MDCE clear,
     * RTS may float and the module stays silent (HCI n=0). */
    jz_write(UART_UMCR(port), BM_UART_UMCR_MDCE | BM_UART_UMCR_RTS);
}

void uart_x1000_modem_bt_release(int port)
{
    if(port < 0 || port > 2)
        return;
    jz_write(UART_UMCR(port), 0);
}

void uart_x1000_set_baud(int port, unsigned baud, unsigned exclk_hz)
{
    unsigned div, umr;
    calc_baud_params(baud, exclk_hz, &div, &umr);

    jz_writef(UART_ULCR(port), DLAB(1));
    jz_write(UART_UDLHR(port), (div >> 8) & 0xff);
    jz_write(UART_UDLLR(port),  div       & 0xff);
    jz_write(UART_UMR(port),   umr);
    jz_write(UART_UACR(port),   0);
    jz_writef(UART_ULCR(port), DLAB(0));
}

void uart_x1000_write(int port, const uint8_t* data, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        /* Wait for TX FIFO to have space (TDRQ = TX data request) */
        while(!jz_readf(UART_ULSR(port), TDRQ))
            yield();
        jz_write(UART_UTHR(port), data[i]);
    }
}

int uart_x1000_write_timed(int port, const uint8_t* data, size_t len,
                           int per_byte_timeout_ms)
{
    for(size_t i = 0; i < len; i++) {
        long deadline = current_tick + (long)per_byte_timeout_ms * HZ / 1000;
        while(!jz_readf(UART_ULSR(port), TDRQ)) {
            if(TIME_AFTER(current_tick, deadline))
                return -1;
            yield();
        }
        jz_write(UART_UTHR(port), data[i]);
    }
    return 0;
}

int uart_x1000_putc(int port, uint8_t c)
{
    if(!jz_readf(UART_ULSR(port), TDRQ))
        return 0;
    jz_write(UART_UTHR(port), c);
    return 1;
}

void uart_x1000_flush(int port)
{
    /* Wait for transmitter empty (TEMP = transmitter empty) */
    while(!jz_readf(UART_ULSR(port), TEMP))
        yield();
}

int uart_x1000_flush_timed(int port, int timeout_ms)
{
    long deadline = current_tick + (long)timeout_ms * HZ / 1000;
    while(!jz_readf(UART_ULSR(port), TEMP)) {
        if(TIME_AFTER(current_tick, deadline))
            return -1;
        yield();
    }
    return 0;
}

/* ---- ISR dispatcher ----
 *
 * The X1000 interrupt controller routes UART0/1/2 to separate IRQ lines.
 * Wire these up in system-x1000.c's interrupt table, or call
 * uart_x1000_isr(port) from a shared handler.
 *
 * The ISR drains the RX FIFO into the ring buffer, then calls rx_cb
 * with the newly arrived bytes so the BT stack can process them without
 * copying through a second buffer.
 */
void uart_x1000_isr(int port)
{
    uart_state_t* s = &uart_state[port];

    /* ULSR_DRY: data ready in RX FIFO */
    while(jz_readf(UART_ULSR(port), DRY)) {
        uint8_t c = jz_read(UART_URBR(port));
        rb_push(&s->rx, c);
    }

    /* Notify listener if one is registered.
     * For BTstack this will be the HCI transport receive pump. */
    if(s->rx_cb && s->rx.head != s->rx.tail) {
        /* Deliver contiguous slice from tail to end-of-buffer or head,
         * whichever comes first, then let the callback loop if needed. */
        size_t tail = s->rx.tail;
        size_t head = s->rx.head;
        size_t end  = (tail < head) ? head : (s->rx.mask + 1);
        s->rx_cb(s->rx.buf + tail, end - tail);
        s->rx.tail = end & s->rx.mask;
        /* Second slice if wrapped */
        if(s->rx.head != s->rx.tail) {
            s->rx_cb(s->rx.buf, s->rx.head);
            s->rx.tail = s->rx.head;
        }
    }
}

/* Convenience ISR entry points — attach to INTC via system-x1000.c */
void UART0(void) { uart_x1000_isr(0); }
void UART1(void) { uart_x1000_isr(1); }
