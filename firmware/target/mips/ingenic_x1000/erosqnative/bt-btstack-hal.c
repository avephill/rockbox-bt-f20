/***************************************************************************
 * BTstack HAL glue for Rockbox on EROS Q Native.
 *
 * Implements:
 *   hal_cpu.h      - IRQ masking for BTstack critical sections
 *   hal_time_ms.h  - millisecond counter derived from current_tick
 *   hal_uart_dma.h - block-based UART interface; bridges to our
 *                    uart-x1000 driver's ISR callback mechanism.
 *
 * BTstack's btstack_uart_block_embedded.c wraps these into a
 * proper btstack_uart_t instance for hci_transport_h4 to consume.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "kernel.h"
#include "uart-x1000.h"
#include "bt-erosqnative.h"

#include "hal_cpu.h"
#include "hal_time_ms.h"
#include "hal_uart_dma.h"

/* ---- hal_cpu ---- */

static int s_irq_level;

void hal_cpu_disable_irqs(void)
{
    s_irq_level = disable_irq_save();
}

void hal_cpu_enable_irqs(void)
{
    restore_irq(s_irq_level);
}

void hal_cpu_enable_irqs_and_sleep(void)
{
    /* No true CPU sleep on X1000 Rockbox — enable IRQs and yield */
    restore_irq(s_irq_level);
    yield();
}

/* ---- hal_time_ms ---- */

uint32_t hal_time_ms(void)
{
    /* current_tick is HZ=100 → 10 ms/tick on this target */
    return (uint32_t)(current_tick * (1000u / HZ));
}

/* ---- hal_uart_dma ---- */

/* Block currently being received, filled in ISR context */
static volatile uint8_t*  rx_target;
static volatile uint16_t  rx_remaining;

/* Callbacks into btstack_uart_block_embedded.c (set during init) */
static void (*cb_block_received)(void) = NULL;
static void (*cb_block_sent)(void)     = NULL;

/* UART-driver ring-buffer backing — sized for one HCI ACL packet */
static uint8_t bt_rxring[BT_UART_RX_BUFSIZE];

/* Invoked from uart-x1000 ISR with a contiguous slice of bytes.
 * Copy into the pending receive block; when full, notify BTstack
 * via the stored block_received hook (which safely defers to the
 * run loop via btstack_run_loop_poll_data_sources_from_irq). */
static void bt_uart_rx_cb(const uint8_t* data, size_t len)
{
    if(rx_target == NULL || rx_remaining == 0)
        return;
    size_t take = (len < rx_remaining) ? len : rx_remaining;
    memcpy((uint8_t*)rx_target, data, take);
    rx_target    += take;
    rx_remaining -= take;
    if(rx_remaining == 0) {
        rx_target = NULL;
        if(cb_block_received)
            cb_block_received();
    }
}

void hal_uart_dma_init(void)
{
    /* UART itself is brought up by bt_hw_power(true). Here we just
     * re-initialize it with our ISR callback so async receive works. */
    uart_x1000_init(BT_UART_PORT, BT_UART_BAUD_INIT, BT_UART_EXCLK_HZ,
                    bt_uart_rx_cb, bt_rxring, sizeof(bt_rxring));
}

void hal_uart_dma_set_block_received(void (*cb)(void))
{
    cb_block_received = cb;
}

void hal_uart_dma_set_block_sent(void (*cb)(void))
{
    cb_block_sent = cb;
}

int hal_uart_dma_set_baud(uint32_t baud)
{
    bt_hw_set_baud((unsigned)baud);
    return 0;
}

int hal_uart_dma_set_flowcontrol(int flowcontrol)
{
    /* We already asserted MDCE|RTS in bt_hw_power(); nothing to do */
    (void)flowcontrol;
    return 0;
}

void hal_uart_dma_send_block(const uint8_t* buffer, uint16_t length)
{
    /* Synchronous write: hci_transport_h4 is prepared for this pattern
     * (it will drain and then block_sent fires). */
    bt_hw_send(buffer, length);
    if(cb_block_sent)
        cb_block_sent();
}

void hal_uart_dma_receive_block(uint8_t* buffer, uint16_t len)
{
    rx_target    = buffer;
    rx_remaining = len;
}

/* Sleep / CSR wake pulses not used for BCM H4 */
void hal_uart_dma_set_csr_irq_handler(void (*h)(void)) { (void)h; }
void hal_uart_dma_set_sleep(uint8_t sleep)             { (void)sleep; }
int  hal_uart_dma_get_supported_sleep_modes(void)      { return 0; }
