/***************************************************************************
 * Crash log with breadcrumbs.
 *
 * Captures the panic message + a ring of recent activity events ("entered
 * Bluetooth menu", "bt: state STREAMING → CONNECTING", "action MENU"...) in
 * a persistent DRAM region, then drains to /.rockbox/crash.log on the next
 * boot. Lets us answer "what was I doing right before it crashed" without
 * the user having to reproduce the path through the UI.
 *
 * The persistent struct sits in its own NOLOAD linker section past the
 * normal BSS clear range, so DRAM contents survive the soft reset that
 * follows a panic. (A full power-cycle empties DRAM and the breadcrumb log
 * is lost — but power-cycles aren't the case we care about here.)
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef __CRASH_LOG_H__
#define __CRASH_LOG_H__

#include <stdint.h>

/* Breadcrumb format. Keep small — we want lots of entries, and writes are
 * called from screen-transition / state-change paths that can fire during
 * playback so they need to be cheap. */
#define CRASH_LOG_NUM_BREADCRUMBS 64
#define CRASH_LOG_MSG_LEN         48
#define CRASH_LOG_PANIC_LEN       128

#define CRASH_LOG_MAGIC           0x43525348u   /* "CRSH" */

struct crash_log_breadcrumb {
    uint32_t tick;
    char     msg[CRASH_LOG_MSG_LEN];
};

enum crash_log_kind {
    CRASH_LOG_KIND_NONE = 0,
    CRASH_LOG_KIND_PANICF,
    CRASH_LOG_KIND_EXCEPTION,
};

struct crash_log_persist {
    uint32_t magic;             /* CRASH_LOG_MAGIC when a captured crash is pending */
    uint32_t kind;              /* enum crash_log_kind */
    uint32_t epc;               /* MIPS EPC at exception time (0 if panicf) */
    uint32_t badvaddr;          /* MIPS BadVAddr (0 if panicf) */
    uint32_t cause;             /* MIPS Cause register (0 if panicf) */
    uint32_t panic_tick;        /* current_tick when capture happened */
    char     panic_msg[CRASH_LOG_PANIC_LEN];

    uint32_t bc_head;           /* next write index modulo NUM_BREADCRUMBS */
    uint32_t bc_count;          /* total breadcrumbs ever written (saturates) */
    struct crash_log_breadcrumb bc[CRASH_LOG_NUM_BREADCRUMBS];
};

/* Reset the breadcrumb ring. Called once early in init() after any pending
 * crash has been drained, so a cleanly-booted device starts with an empty
 * ring rather than residual bits from whatever was running before. */
void crash_log_reset(void);

/* If a captured crash is pending (magic word set), open /.rockbox/crash.log
 * in append mode, write the panic + breadcrumb ring, and clear the magic.
 * Safe to call from regular thread context — the actual file I/O happens
 * here, NOT during the exception. Idempotent: returns 0 if nothing pending. */
int  crash_log_drain_to_file(void);

/* Append a breadcrumb. Intended for screen transitions, state changes,
 * action receipts. Truncates the message if it overflows CRASH_LOG_MSG_LEN.
 * Cheap (a few memcpys) and safe to call from any context that isn't the
 * exception handler itself. */
void crash_log_breadcrumb(const char* msg);
/* printf-style variant for messages with a couple of dynamic fields. */
void crash_log_breadcrumbf(const char* fmt, ...) __attribute__((format(printf,1,2)));

/* Capture entry points. Called from panic.c / system-mips.c right before
 * the panic LCD render + system_reboot. Sets the persistent magic so the
 * next boot drains the log. */
void crash_log_capture_panicf(const char* msg);
void crash_log_capture_exception(const char* msg, uint32_t epc,
                                 uint32_t badvaddr, uint32_t cause);

#endif /* __CRASH_LOG_H__ */
