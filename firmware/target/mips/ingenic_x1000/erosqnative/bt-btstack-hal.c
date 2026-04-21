/***************************************************************************
 * BTstack HAL glue for Rockbox on EROS Q Native.
 *
 * Implements hal_cpu.h (IRQ masking) and hal_time_ms.h (ms counter).
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#include "system.h"
#include "kernel.h"
#include "hal_cpu.h"
#include "hal_time_ms.h"

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
    /* No real sleep on X1000 bare-metal — just re-enable and yield */
    restore_irq(s_irq_level);
    yield();
}

uint32_t hal_time_ms(void)
{
    /* current_tick is HZ=100 (10 ms/tick on this target) */
    return (uint32_t)(current_tick * (1000u / HZ));
}
