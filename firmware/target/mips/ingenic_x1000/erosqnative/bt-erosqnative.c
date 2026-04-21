/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * BT transport for EROS Q Native / Surfans F20 (HW4) — BCM4343A1.
 *
 * Copyright (C) 2024-2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/

#include "bt-erosqnative.h"
#include "uart-x1000.h"
#include "gpio-x1000.h"
#include "kernel.h"
#include "system.h"
#include "x1000/uart.h"
#include "x1000/gpio.h"

/* Pin assignments (HW4) */
#define PIN_BT_REG_ON       GPIO_PC(18)
#define PIN_WL_REG_ON       GPIO_PC(21)
#define PIN_HOST_WAKE       GPIO_PC(19)
#define PIN_CHIP_WAKE       GPIO_PC(20)
#define PIN_CLK32K          GPIO_PB(26)   /* func0 = CLK32K output from RTC */

#define UART0_PC_MASK       ((1u << 10) | (1u << 11) | (1u << 12) | (1u << 13))
#define UART0_PC_FUNC       GPIOF_DEVICE(0)

/* RX ring buffer — owned by the UART driver. Not used while in polled mode. */
static uint8_t bt_rx_buf[BT_UART_RX_BUFSIZE];

/* ---- internal GPIO helper ---- */

/* Atomically drive a GPIO HIGH by resetting INT/MASK/PAT1/PAT0 via GPIO Z
 * hold-and-load. Required because gpio_set_level() only writes PAT0 and
 * fails if the pin is in input or interrupt mode from the bootloader. */
static void gpio_force_high(int gpio)
{
    int port = GPION_PORT(gpio);
    uint32_t bit = GPION_MASK(gpio);
    jz_clr(GPIO_INT(GPIO_Z),  bit);
    jz_set(GPIO_MSK(GPIO_Z),  bit);
    jz_clr(GPIO_PAT1(GPIO_Z), bit);
    jz_set(GPIO_PAT0(GPIO_Z), bit);
    REG_GPIO_Z_GID2LD = port;
}

/* ---- public transport API ---- */

int bt_hw_power(bool on)
{
    if(on) {
        /* 1. Enable 32.768 kHz LPO clock — PB26 func0 = CLK32K output.
         *    Without this the BCM chip's ROM cannot run. */
        gpioz_configure(GPIO_B, 1u << 26, GPIOF_DEVICE(0));
        mdelay(10);

        /* 2. Start with chip off: all BT control pins driven LOW, UART
         *    pins as input so we don't fight the chip during boot. */
        gpioz_configure(GPIO_C,
                        (1u << 18) | (1u << 19) | (1u << 21),
                        GPIOF_OUTPUT(0));
        gpioz_configure(GPIO_C, UART0_PC_MASK, GPIOF_INPUT);
        mdelay(200);

        /* 3. Power sequence matching stock kernel: WL first, then BT core,
         *    then HOST_WAKE. Delays verified empirically. */
        gpio_force_high(PIN_WL_REG_ON);
        mdelay(63);
        gpio_force_high(PIN_BT_REG_ON);
        mdelay(150);    /* ROM boot */
        gpio_force_high(PIN_HOST_WAKE);
        mdelay(50);

        /* 4. Mux UART0 to PC10-13 and initialize at 115200 8N1 (no callback;
         *    polled mode for patchram). Enable hardware flow control (MDCE+RTS). */
        gpioz_configure(GPIO_C, UART0_PC_MASK, UART0_PC_FUNC);
        uart_x1000_init(BT_UART_PORT, BT_UART_BAUD_INIT, BT_UART_EXCLK_HZ,
                        NULL, bt_rx_buf, sizeof(bt_rx_buf));
        uart_x1000_modem_bt_host_ready(BT_UART_PORT);
        mdelay(5);
    } else {
        uart_x1000_modem_bt_release(BT_UART_PORT);

        /* Release UART pins so we don't hold lines against a powered-down chip */
        gpioz_configure(GPIO_C, UART0_PC_MASK, GPIOF_INPUT);

        /* Drop all BT control lines */
        gpioz_configure(GPIO_C,
                        (1u << 18) | (1u << 19) | (1u << 21),
                        GPIOF_OUTPUT(0));

        /* Release CLK32K (decremented refcount in kernel; here just release) */
        gpioz_configure(GPIO_B, 1u << 26, GPIOF_INPUT);
        mdelay(10);
    }
    return 0;
}

int bt_hw_send(const uint8_t* buf, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        long dl = current_tick + 500 * HZ / 1000;
        while(!jz_readf(UART_ULSR(BT_UART_PORT), TDRQ)) {
            if(TIME_AFTER(current_tick, dl)) return -1;
            yield();
        }
        jz_write(UART_UTHR(BT_UART_PORT), buf[i]);
    }
    long fdl = current_tick + 1000 * HZ / 1000;
    while(!jz_readf(UART_ULSR(BT_UART_PORT), TEMP)) {
        if(TIME_AFTER(current_tick, fdl)) return -1;
        yield();
    }
    return 0;
}

int bt_hw_recv(uint8_t* buf, size_t max, int first_byte_ms)
{
    int n = 0;
    int tmo = first_byte_ms;
    for(size_t i = 0; i < max; i++) {
        long dl = current_tick + (long)tmo * HZ / 1000;
        while(!jz_readf(UART_ULSR(BT_UART_PORT), DRY)) {
            if(TIME_AFTER(current_tick, dl))
                return n;
            yield();
        }
        buf[n++] = (uint8_t)jz_read(UART_URBR(BT_UART_PORT));
        tmo = 200;   /* subsequent bytes: 200ms */
    }
    return n;
}

void bt_hw_drain_rx(void)
{
    while(jz_readf(UART_ULSR(BT_UART_PORT), DRY))
        (void)jz_read(UART_URBR(BT_UART_PORT));
}

void bt_hw_set_baud(unsigned baud)
{
    uart_x1000_flush(BT_UART_PORT);
    uart_x1000_set_baud(BT_UART_PORT, baud, BT_UART_EXCLK_HZ);
    mdelay(5);
}

bool bt_chip_is_awake(void)
{
    return (jz_read(GPIO_PIN(GPIO_C)) & (1u << 20)) != 0;
}
