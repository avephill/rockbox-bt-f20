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
 * Supports UART0, UART1, UART2. UART2 is already used for debug output
 * via the bootloader; do not re-init it here. UART1 is used for BT HCI.
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/
#ifndef _UART_X1000_H
#define _UART_X1000_H

#include <stdint.h>
#include <stddef.h>

/* Notification invoked from interrupt context after the ISR has pushed
 * newly received bytes into the ring. The listener should drain what it
 * needs via uart_x1000_rx_read(); bytes remain in the ring until popped. */
typedef void (*uart_rx_notify_t)(int port);

/* Initialise a UART port.
 *
 *   port      — 0, 1, or 2
 *   baud      — baud rate (e.g. 115200)
 *   exclk_hz  — X1000 external clock frequency (24 000 000 for all known boards)
 *   rx_notify — called from ISR after bytes have been pushed to the ring;
 *               may be NULL for TX-only / polled-RX use
 *   rx_buf    — caller-provided ring buffer (must be power-of-2 in size)
 *   rx_bufsz  — size of rx_buf in bytes (must be power of 2)
 */
void uart_x1000_init(int port, unsigned baud, unsigned exclk_hz,
                     uart_rx_notify_t rx_notify,
                     uint8_t* rx_buf, size_t rx_bufsz);

/* Pop up to `max` bytes from the RX ring into `buf`. Returns the number
 * actually copied (0 if ring is empty). Safe to call from both thread
 * and IRQ context. */
size_t uart_x1000_rx_read(int port, uint8_t* buf, size_t max);

/* Change baud rate without full re-init (used after RTL firmware upload). */
void uart_x1000_set_baud(int port, unsigned baud, unsigned exclk_hz);

/* Blocking transmit — waits for FIFO space before each byte.
 * Safe to call from any context; does not use interrupts. */
void uart_x1000_write(int port, const uint8_t* data, size_t len);

/* Like uart_x1000_write / uart_x1000_flush but return -1 if TDRQ / TEMP
 * never assert within the deadline (yields while waiting). */
int uart_x1000_write_timed(int port, const uint8_t* data, size_t len,
                           int per_byte_timeout_ms);
int uart_x1000_flush_timed(int port, int timeout_ms);

/* Non-blocking single-byte transmit; returns 0 if FIFO was full. */
int  uart_x1000_putc(int port, uint8_t c);

/* Flush TX FIFO (spin until shift register empty). */
void uart_x1000_flush(int port);

/* Drive host RTS so a BT module whose CTS is wired to host RTS can transmit.
 * Sets UMCR: MDCE (modem pin output enable) + RTS asserted. Call after
 * uart_x1000_init() for HCI on UART1. Do not use on UART2 if pins are unused. */
void uart_x1000_modem_bt_host_ready(int port);

/* Clear UMCR (call when powering BT down). */
void uart_x1000_modem_bt_release(int port);

#endif /* _UART_X1000_H */
