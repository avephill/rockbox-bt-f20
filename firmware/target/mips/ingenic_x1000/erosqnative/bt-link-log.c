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
#include "file.h"
#include "bt-link-log.h"

/* 2048 x 64 B = 128 KB of ring — cheap on this target, and sized so a
 * full outdoor walk between pause-flushes fits without dropping (the
 * 256-entry ring lost up to ~1700 lines per walk session, which kept
 * costing us the interesting stretch of every diagnosis). Must be a
 * power of two. */
#define LOG_CAP 2048

/* Cap the on-card log so append-across-reboots can't fill the card over
 * months. When the file passes this, the next dump rotates it (truncate +
 * fresh "=== log start ===" header). Sized to hold a handful of full-ring
 * dumps (a full ring is ~140 KB on card). */
#define LOG_MAX_BYTES (1024 * 1024)

struct entry {
    uint32_t ts_ms;
    char     msg[BT_LINK_LOG_MSG_LEN];
};

static struct entry s_ring[LOG_CAP];
static uint32_t     s_total;      /* monotonic; modulo LOG_CAP gives next-write index */

/* Logging is opt-in (toggled from the Bluetooth screen) so the ring isn't
 * filled and the card isn't written during normal use. RAM-only: resets to
 * off on every boot. */
static bool s_enabled;
/* How many total entries have already been flushed to the file, so each dump
 * appends only what's new since the last one instead of re-writing the ring
 * (which would clobber or duplicate across multiple pauses). */
static uint32_t s_dumped;
/* Write a "=== log start ===" session header on the next dump that has
 * content. Set at boot, on enable, on reset, and after a rotate. */
static bool s_need_header = true;

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
    s_dumped = 0;
    s_need_header = true;   /* next dump starts a fresh session section */
    memset(s_ring, 0, sizeof(s_ring));
    restore_irq(irq);
}

bool bt_link_log_enabled(void)
{
    return s_enabled;
}

void bt_link_log_set_enabled(bool en)
{
    int irq = disable_irq_save();
    if(en && !s_enabled) {
        /* Start clean so the captured window is just this session. */
        s_total = 0;
        s_dumped = 0;
        s_need_header = true;
        memset(s_ring, 0, sizeof(s_ring));
    }
    s_enabled = en;
    restore_irq(irq);
}

void bt_link_logf(const char* fmt, ...)
{
    if(!s_enabled) return;
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

void bt_link_log_dump(const char* path)
{
    if(!s_enabled) return;

    /* Single-threaded against bt_link_logf (both run on the BT thread), so
     * no new entries can land mid-dump; s_total/s_ring are stable here. */
    uint32_t t    = s_total;
    int      cnt  = t < LOG_CAP ? (int)t : LOG_CAP;
    uint32_t base = t - (uint32_t)cnt;          /* global index of oldest live entry */
    uint32_t start = s_dumped > base ? s_dumped : base;
    if(start >= t) return;                       /* nothing new since last flush */

    /* Append so multiple pauses (and reboots) accumulate into one file
     * instead of clobbering each other. */
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if(fd < 0) return;

    /* Rotate if the file has grown past the cap. */
    if(lseek(fd, 0, SEEK_END) > LOG_MAX_BYTES) {
        close(fd);
        fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if(fd < 0) return;
        s_need_header = true;
    }

    char line[BT_LINK_LOG_MSG_LEN + 24];
    int n;
    if(s_need_header) {
        n = snprintf(line, sizeof(line), "=== log start ===\n");
        s_need_header = false;
    } else {
        n = snprintf(line, sizeof(line), "--- pause ---\n");
    }
    if(n > 0) write(fd, line, n);

    /* Note any entries that wrapped out of the ring before we could flush. */
    if(base > s_dumped) {
        n = snprintf(line, sizeof(line), "... %lu dropped ...\n",
                     (unsigned long)(base - s_dumped));
        if(n > 0) write(fd, line, n);
    }

    for(uint32_t g = start; g < t; g++) {
        uint32_t ts = 0;
        char msg[BT_LINK_LOG_MSG_LEN];
        if(!bt_link_log_get((int)(g - base), &ts, msg, sizeof(msg))) continue;
        n = snprintf(line, sizeof(line), "%lu.%02u %s\n",
                     (unsigned long)(ts / 1000),
                     (unsigned)((ts % 1000) / 10), msg);
        if(n > 0) write(fd, line, n);
    }
    close(fd);
    s_dumped = t;
}

#endif /* !BOOTLOADER */
