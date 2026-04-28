/***************************************************************************
 * BT BTstack HAL — public API.
 *
 * Used by the dedicated BT thread setup in bt-a2dp.c to seed the wakeup
 * semaphore that hal_cpu_enable_irqs_and_sleep blocks on. Also exposes a
 * way for the foreground UI to nudge the BT thread when it posts an input
 * action.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

/* Initialize the BT-thread wakeup semaphore. Must be called before the BT
 * thread starts running btstack_run_loop_embedded_execute_once. */
void bt_btstack_hal_init(void);

/* Release the BT-thread wakeup semaphore. Safe from ISR or any thread. The
 * UART RX notify hook calls this; the foreground UI calls it after posting
 * an input action so the BT thread picks it up immediately. */
void bt_btstack_hal_signal(void);
