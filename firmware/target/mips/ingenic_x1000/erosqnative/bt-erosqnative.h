/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Bluetooth hardware transport for EROS Q Native / Surfans F20 (HW4).
 *
 * Chip: BCM4343A1 (Broadcom/Cypress CYW43438) — combo WiFi+BT. BT via UART0
 * on Port C func0 (PC10=TXD, PC11=RXD, PC12=CTS, PC13=RTS). Power/wake on
 * PC18=BT_REG_ON, PC19=HOST_WAKE, PC20=CHIP_WAKE(in), PC21=WL_REG_ON.
 * Reference 32.768 kHz LPO clock is sourced from the X1000 RTC oscillator
 * routed through PB26 device function 0 (CLK32K output).
 *
 * Copyright (C) 2024-2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/
#ifndef _BT_EROSQNATIVE_H
#define _BT_EROSQNATIVE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Hardware parameters ---- */
#define BT_UART_PORT        0
#define BT_UART_BAUD_INIT   115200
#define BT_UART_BAUD_HS     3000000
#define BT_UART_EXCLK_HZ    24000000
#define BT_UART_RX_BUFSIZE  4096

/* BCM patchram firmware file on SD card (.hcd format from stock rootfs) */
#define BT_BCM_FW_PATH      "/.rockbox/BCM4343A1.hcd"

/* ---- Transport API (polled byte pipe, single-threaded) ---- */

/* Bring the chip from off to the state where the ROM bootloader responds
 * to HCI. Idempotent-ish: calling with on=false returns the chip to cold.
 * When on=true: enables PB26 CLK32K, drives WL_REG_ON→BT_REG_ON→HOST_WAKE,
 * muxes UART0 to PC10-13 at 115200 8N1, and asserts host-side RTS with MDCE.
 * Returns 0 on success. */
int  bt_hw_power(bool on);

/* Send raw bytes. Blocks until the TX FIFO has drained. Returns 0 or -1. */
int  bt_hw_send(const uint8_t* buf, size_t len);

/* Read up to `max` bytes. Waits up to `first_byte_ms` for the first byte,
 * then uses 200ms between subsequent bytes. Returns byte count (>=0). */
int  bt_hw_recv(uint8_t* buf, size_t max, int first_byte_ms);

/* Discard any bytes waiting in the RX FIFO. */
void bt_hw_drain_rx(void);

/* Change UART baud rate (e.g. after vendor Update_Baudrate command). */
void bt_hw_set_baud(unsigned baud);

/* Read the CHIP_WAKE (PC20) pin — chip asserts to wake host. */
bool bt_chip_is_awake(void);

#endif /* _BT_EROSQNATIVE_H */
