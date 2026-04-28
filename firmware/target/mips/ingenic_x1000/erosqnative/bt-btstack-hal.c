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
#include "semaphore.h"
#include "uart-x1000.h"
#include "bt-erosqnative.h"
#include "bt-btstack-hal.h"

#include "hal_cpu.h"
#include "hal_time_ms.h"
#include "hal_uart_dma.h"

#include "btstack_run_loop.h"

/* ---- hal_cpu ---- */

static int s_irq_level;

/* The BT thread sleeps on this semaphore between run-loop iterations. The
 * UART RX ISR (and the foreground UI when posting input) signals it; the
 * timer-due delta computed from BTstack's pending-timer list bounds the
 * wait. Without a real sleep primitive the run loop spins, codec/UI threads
 * starve, and execute_once iterates only when other threads happen to yield
 * — which on this target gave ~19 iterations/s and capped A2DP throughput. */
static struct semaphore s_bt_wakeup;
static bool             s_bt_wakeup_inited;

void bt_btstack_hal_init(void)
{
    if (s_bt_wakeup_inited) return;
    semaphore_init(&s_bt_wakeup, 1, 0);
    s_bt_wakeup_inited = true;
}

void bt_btstack_hal_signal(void)
{
    if (s_bt_wakeup_inited)
        semaphore_release(&s_bt_wakeup);
}

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
    restore_irq(s_irq_level);
    if (!s_bt_wakeup_inited) {
        yield();
        return;
    }
    /* Sleep until either an ISR/foreground signal or the next BTstack timer
     * is due. Round up ms→ticks; cap at HZ to avoid sleeping past wakeups
     * we might miss. semaphore_wait is woken immediately by semaphore_release. */
    uint32_t now = hal_time_ms();
    int32_t timeout_ms = btstack_run_loop_base_get_time_until_timeout(now);
    int timeout_ticks;
    if (timeout_ms < 0) {
        timeout_ticks = HZ;          /* no timer pending — sleep up to 1s */
    } else if (timeout_ms == 0) {
        return;                       /* a timer is already due — don't sleep */
    } else {
        timeout_ticks = (timeout_ms * HZ + 999) / 1000;
        if (timeout_ticks <= 0) timeout_ticks = 1;
        if (timeout_ticks > HZ)  timeout_ticks = HZ;
    }
    semaphore_wait(&s_bt_wakeup, timeout_ticks);
}

/* ---- hal_time_ms ---- */

uint32_t hal_time_ms(void)
{
    /* current_tick is HZ=100 → 10 ms/tick on this target */
    return (uint32_t)(current_tick * (1000u / HZ));
}

/* ---- hal_uart_dma ---- */

/* Block currently being received. rx_target != NULL means BTstack has a
 * pending receive_block; the ISR notification drains the ring into it. */
static volatile uint8_t*  rx_target;
static volatile uint16_t  rx_remaining;

/* Callbacks into btstack_uart_block_embedded.c (set during init) */
static void (*cb_block_received)(void) = NULL;
static void (*cb_block_sent)(void)     = NULL;

/* UART-driver ring-buffer backing — sized for one HCI ACL packet */
static uint8_t bt_rxring[BT_UART_RX_BUFSIZE];

/* Current UART baud. Initialized to the post-power-on rate; updated when
 * BTstack (or our pre-init manual switch) calls hal_uart_dma_set_baud, so
 * that subsequent hal_uart_dma_init re-init calls don't clobber a faster
 * rate back down to the init baud. */
static unsigned s_current_baud = BT_UART_BAUD_INIT;

/* Drain the UART ring into the pending receive block. Fires block_received
 * when full. Called both from the UART ISR (via bt_uart_rx_notify) and
 * from hal_uart_dma_receive_block (in case bytes already arrived). */
static void bt_uart_drain_to_target(void)
{
    if(rx_target == NULL || rx_remaining == 0)
        return;
    size_t n = uart_x1000_rx_read(BT_UART_PORT,
                                  (uint8_t*)rx_target, rx_remaining);
    rx_target    += n;
    rx_remaining -= n;
    if(rx_remaining == 0) {
        rx_target = NULL;
        if(cb_block_received)
            cb_block_received();
    }
}

static void bt_uart_rx_notify(int port)
{
    (void)port;
    bt_uart_drain_to_target();
    /* Wake the BT thread so its run-loop iteration runs immediately rather
     * than waiting for the next-timer timeout in hal_cpu_enable_irqs_and_sleep. */
    bt_btstack_hal_signal();
}

void hal_uart_dma_init(void)
{
    /* UART itself is brought up by bt_hw_power(true). Here we just
     * re-initialize with our ring-drain notification so async receive works.
     * Use s_current_baud rather than the init constant so a prior baud
     * switch (e.g. to 3 Mbps via vendor cmd 0xFC18) survives this re-init. */
    uart_x1000_init(BT_UART_PORT, s_current_baud, BT_UART_EXCLK_HZ,
                    bt_uart_rx_notify, bt_rxring, sizeof(bt_rxring));
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
    s_current_baud = (unsigned)baud;
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
    int old = disable_irq_save();
    rx_target    = buffer;
    rx_remaining = len;
    bt_uart_drain_to_target();  /* in case bytes already in ring */
    restore_irq(old);
}

/* Sleep / CSR wake pulses not used for BCM H4 */
void hal_uart_dma_set_csr_irq_handler(void (*h)(void)) { (void)h; }
void hal_uart_dma_set_sleep(uint8_t sleep)             { (void)sleep; }
int  hal_uart_dma_get_supported_sleep_modes(void)      { return 0; }
