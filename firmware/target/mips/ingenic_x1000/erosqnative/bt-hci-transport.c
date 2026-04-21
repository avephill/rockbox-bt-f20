/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * BTstack HCI UART transport for EROS Q Native / Surfans F20
 *
 * Implements btstack_uart_t so BTstack can send/receive HCI packets
 * through our X1000 UART1 driver.
 *
 * BTstack must be cloned into firmware/drivers/btstack/ and built as
 * a static library. Required BTstack components:
 *   - src/btstack_uart.h          (interface this file implements)
 *   - src/hci_transport_h4.c      (H4 framing — include in build)
 *   - src/btstack_run_loop.h      (run loop — use embedded/btstack_run_loop_embedded.c)
 *
 * License note: BTstack is free for non-commercial use. This file is
 * GPLv2 but links against BTstack under its non-commercial terms.
 * Cannot be upstreamed to Rockbox without a GPL-compatible BTstack license.
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/

#include "bt-erosqnative.h"
#include "uart-x1000.h"
#include "kernel.h"

/* BTstack headers — available after cloning BTstack into firmware/drivers/btstack */
#include "btstack_uart.h"           /* btstack_uart_t interface */
#include "btstack_run_loop.h"
#include "hci.h"

/* ---- state ---- */

static void (*s_block_received)(void)  = NULL;
static void (*s_block_sent)(void)      = NULL;

static const uint8_t* s_tx_buf  = NULL;
static size_t         s_tx_len  = 0;

/* Pending RX block (set by BTstack before each receive) */
static uint8_t* s_rx_buf  = NULL;
static size_t   s_rx_len  = 0;
static size_t   s_rx_done = 0;

/* ---- called from uart_x1000_isr() → bt_hci_rx_callback() ---- */

void bt_hci_rx_callback(const uint8_t* data, size_t len)
{
    if(!s_rx_buf || s_rx_done >= s_rx_len)
        return;

    size_t want = s_rx_len - s_rx_done;
    size_t take = (len < want) ? len : want;

    for(size_t i = 0; i < take; i++)
        s_rx_buf[s_rx_done++] = data[i];

    if(s_rx_done >= s_rx_len && s_block_received) {
        s_rx_buf  = NULL;
        s_rx_done = 0;
        s_block_received();   /* notify BTstack H4 layer */
    }
}

/* ---- btstack_uart_t implementation ---- */

static int transport_init(const btstack_uart_config_t* config)
{
    (void)config;
    bt_hw_init();
    return 0;
}

static int transport_open(void)
{
    bt_hw_power(1);
    /* RTL8723DS firmware upload happens here before returning.
     * See bt-rtl8723ds.c (to be written) for the upload sequence. */
    return 0;
}

static int transport_close(void)
{
    bt_hw_power(0);
    return 0;
}

static void transport_set_block_received(void (*cb)(void))
{
    s_block_received = cb;
}

static void transport_set_block_sent(void (*cb)(void))
{
    s_block_sent = cb;
}

static void transport_receive_block(uint8_t* buf, uint16_t len)
{
    s_rx_buf  = buf;
    s_rx_len  = len;
    s_rx_done = 0;
}

static void transport_send_block(const uint8_t* buf, uint16_t len)
{
    s_tx_buf = buf;
    s_tx_len = len;

    uart_x1000_write(BT_UART_PORT, buf, len);
    uart_x1000_flush(BT_UART_PORT);

    s_tx_buf = NULL;
    s_tx_len = 0;

    if(s_block_sent)
        s_block_sent();
}

static int transport_set_baud(uint32_t baud)
{
    uart_x1000_flush(BT_UART_PORT);
    uart_x1000_set_baud(BT_UART_PORT, baud, BT_UART_EXCLK_HZ);
    return 0;
}

/* Exported btstack_uart_t instance */
static const btstack_uart_t bt_uart_transport = {
    .init              = transport_init,
    .open              = transport_open,
    .close             = transport_close,
    .set_block_received = transport_set_block_received,
    .set_block_sent    = transport_set_block_sent,
    .receive_block     = transport_receive_block,
    .send_block        = transport_send_block,
    .set_baud          = transport_set_baud,
};

const btstack_uart_t* bt_get_uart_transport(void)
{
    return &bt_uart_transport;
}
