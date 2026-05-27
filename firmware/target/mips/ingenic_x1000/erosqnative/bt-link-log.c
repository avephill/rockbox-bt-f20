/***************************************************************************
 * BT link log — see bt-link-log.h.
 *
 * Ring-buffer of (timestamp, short message) records. Writer is the BT
 * thread (and the pcm sink, also BT-thread context). Readers are foreground
 * UI threads, which snapshot under disable_irq_save() to avoid tearing
 * against an in-flight bt_link_logf().
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef BOOTLOADER

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "bt-link-log.h"

#define LOG_CAP 64    /* must be a power of two */

struct entry {
    uint32_t ts_ms;
    char     msg[BT_LINK_LOG_MSG_LEN];
};

static struct entry s_ring[LOG_CAP];
static uint32_t     s_total;      /* monotonic; modulo LOG_CAP gives next-write index */

static uint32_t now_ms(void)
{
    /* current_tick is in HZ ticks; on F20 HZ=100 → 10 ms steps. Match the
     * units bt-pcm-sink uses for its send timing so timestamps align. */
    return (uint32_t)current_tick * (1000u / HZ);
}

void bt_link_log_reset(void)
{
    int irq = disable_irq_save();
    s_total = 0;
    memset(s_ring, 0, sizeof(s_ring));
    restore_irq(irq);
}

void bt_link_logf(const char* fmt, ...)
{
    /* Reserve our slot, then format directly into it. IRQ-disable bracket
     * keeps a concurrent snapshot from racing the message write. */
    int irq = disable_irq_save();
    uint32_t idx = s_total++;
    struct entry* e = &s_ring[idx & (LOG_CAP - 1)];
    e->ts_ms = now_ms();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    restore_irq(irq);
}

int bt_link_log_count(void)
{
    uint32_t t = s_total;
    return t < LOG_CAP ? (int)t : LOG_CAP;
}

uint32_t bt_link_log_total(void)
{
    return s_total;
}

bool bt_link_log_get(int i, uint32_t* ts_ms, char* out, size_t out_sz)
{
    int irq = disable_irq_save();
    uint32_t t = s_total;
    int cnt = t < LOG_CAP ? (int)t : LOG_CAP;
    if(i < 0 || i >= cnt) { restore_irq(irq); return false; }
    /* Oldest live entry sits at (t - cnt) modulo cap. */
    uint32_t base = t - (uint32_t)cnt;
    struct entry* e = &s_ring[(base + (uint32_t)i) & (LOG_CAP - 1)];
    if(ts_ms) *ts_ms = e->ts_ms;
    if(out && out_sz) {
        size_t n = strlen(e->msg);
        if(n >= out_sz) n = out_sz - 1;
        memcpy(out, e->msg, n);
        out[n] = '\0';
    }
    restore_irq(irq);
    return true;
}

#endif /* !BOOTLOADER */
