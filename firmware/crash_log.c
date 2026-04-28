/***************************************************************************
 * Crash log — see crash_log.h.
 *
 * Threading: capture functions (capture_panicf, capture_exception) run from
 * panic context with IRQs disabled and may not call into the filesystem.
 * They touch only the persistent struct in DRAM (memcpy + a couple of word
 * writes). The drain-to-file step runs from regular thread context during
 * main() init, well after the kernel and storage are up.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#ifndef BOOTLOADER

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "crash_log.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "version.h"

extern const char rbversion[];

/* The persistent struct. Placed in the NOLOAD .crash_log section so crt0
 * doesn't zero it. After power-on without a soft-reset path through it,
 * DRAM contains arbitrary garbage — the magic check in drain_to_file
 * filters that out. */
__attribute__((section(".crash_log"), used))
struct crash_log_persist crash_log_persist;

#define CRASH_LOG_PATH ROCKBOX_DIR "/crash.log"

/* ---- breadcrumb ring (writers) ---- */

static void copy_msg(char* dst, const char* src)
{
    /* memcpy + manual NUL — we don't want strncpy's trailing-zero fill of
     * the whole buffer (waste of cycles in a hot path). */
    size_t i = 0;
    while (i + 1 < CRASH_LOG_MSG_LEN && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void crash_log_breadcrumb(const char* msg)
{
    if (!msg) return;
    /* No locking. Writes from preemptable contexts can race with each
     * other but the worst case is a slightly-mangled breadcrumb in the
     * ring — we'd rather have that than block a hot path. The bc_head
     * read-then-write is racy too but the ring overwrites old entries
     * anyway, so a duplicate slot or a skipped slot is still only a
     * minor distortion of the timeline. */
    uint32_t idx = crash_log_persist.bc_head % CRASH_LOG_NUM_BREADCRUMBS;
    crash_log_persist.bc[idx].tick = (uint32_t)current_tick;
    copy_msg(crash_log_persist.bc[idx].msg, msg);
    crash_log_persist.bc_head = idx + 1;
    if (crash_log_persist.bc_count < UINT32_MAX)
        crash_log_persist.bc_count++;
}

void crash_log_breadcrumbf(const char* fmt, ...)
{
    char buf[CRASH_LOG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    crash_log_breadcrumb(buf);
}

void crash_log_reset(void)
{
    /* Clear breadcrumbs but DON'T touch crash_log_persist.magic — that's
     * cleared by drain_to_file once the contents have been safely written
     * out. Resetting between drain and reset would lose the data. */
    crash_log_persist.bc_head  = 0;
    crash_log_persist.bc_count = 0;
    memset(crash_log_persist.bc, 0, sizeof(crash_log_persist.bc));
}

/* ---- capture (panic context — keep this minimal!) ---- */

void crash_log_capture_panicf(const char* msg)
{
    crash_log_persist.kind     = CRASH_LOG_KIND_PANICF;
    crash_log_persist.epc      = 0;
    crash_log_persist.badvaddr = 0;
    crash_log_persist.cause    = 0;
    crash_log_persist.panic_tick = (uint32_t)current_tick;
    if (msg) copy_msg(crash_log_persist.panic_msg, msg);
    else     crash_log_persist.panic_msg[0] = '\0';
    /* Magic written LAST so a partially-completed capture (interrupted by
     * a watchdog reset, say) is filtered out by drain_to_file. */
    crash_log_persist.magic = CRASH_LOG_MAGIC;
}

void crash_log_capture_exception(const char* msg, uint32_t epc,
                                 uint32_t badvaddr, uint32_t cause)
{
    crash_log_persist.kind     = CRASH_LOG_KIND_EXCEPTION;
    crash_log_persist.epc      = epc;
    crash_log_persist.badvaddr = badvaddr;
    crash_log_persist.cause    = cause;
    crash_log_persist.panic_tick = (uint32_t)current_tick;
    if (msg) copy_msg(crash_log_persist.panic_msg, msg);
    else     crash_log_persist.panic_msg[0] = '\0';
    crash_log_persist.magic = CRASH_LOG_MAGIC;
}

/* ---- drain (regular thread context, post-boot) ---- */

static void write_str(int fd, const char* s)
{
    write(fd, s, strlen(s));
}

int crash_log_drain_to_file(void)
{
    if (crash_log_persist.magic != CRASH_LOG_MAGIC) return 0;

    int fd = open(CRASH_LOG_PATH, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd < 0) {
        /* Don't clear the magic — try again next boot. The user likely
         * has the disk mounted by USB or storage isn't ready yet. */
        return -1;
    }

    char line[160];
    int n;
    const char* kind = crash_log_persist.kind == CRASH_LOG_KIND_EXCEPTION
                         ? "exception" : "panicf";

    n = snprintf(line, sizeof(line),
                 "\n=== Crash captured (%s, build %s) ===\n",
                 kind, rbversion);
    write(fd, line, n);

    n = snprintf(line, sizeof(line),
                 "tick: %lu\nmsg : %.*s\n",
                 (unsigned long)crash_log_persist.panic_tick,
                 (int)CRASH_LOG_PANIC_LEN, crash_log_persist.panic_msg);
    write(fd, line, n);

    if (crash_log_persist.kind == CRASH_LOG_KIND_EXCEPTION) {
        n = snprintf(line, sizeof(line),
                     "epc=%08lx badvaddr=%08lx cause=%08lx\n",
                     (unsigned long)crash_log_persist.epc,
                     (unsigned long)crash_log_persist.badvaddr,
                     (unsigned long)crash_log_persist.cause);
        write(fd, line, n);
    }

    /* Walk breadcrumbs in chronological order. bc_head points at the
     * next write slot; the entry just behind it is the newest. If
     * bc_count < NUM, ring isn't full yet — start from index 0. */
    uint32_t total = crash_log_persist.bc_count;
    if (total > CRASH_LOG_NUM_BREADCRUMBS) total = CRASH_LOG_NUM_BREADCRUMBS;
    uint32_t start = (total == CRASH_LOG_NUM_BREADCRUMBS)
                       ? crash_log_persist.bc_head
                       : 0;
    write_str(fd, "Breadcrumbs (oldest -> newest):\n");
    for (uint32_t i = 0; i < total; i++) {
        uint32_t idx = (start + i) % CRASH_LOG_NUM_BREADCRUMBS;
        n = snprintf(line, sizeof(line),
                     "[t=%10lu] %.*s\n",
                     (unsigned long)crash_log_persist.bc[idx].tick,
                     (int)CRASH_LOG_MSG_LEN,
                     crash_log_persist.bc[idx].msg);
        write(fd, line, n);
    }
    if (total == 0) write_str(fd, "  (none)\n");

    write_str(fd, "=== end ===\n");
    close(fd);

    /* Clear magic so we don't re-emit on the next boot. Leave the
     * breadcrumb ring intact for now — crash_log_reset clears it
     * separately so a clean boot starts with an empty ring. */
    crash_log_persist.magic = 0;
    return 1;
}

#endif /* !BOOTLOADER */
