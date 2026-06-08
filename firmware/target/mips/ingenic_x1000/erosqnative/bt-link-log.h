/***************************************************************************
 * BT link log — small ring buffer of timestamped diagnostic events.
 *
 * Built for tracing audio cutouts to specific BT controller / link events
 * (mode change, role change, packet-type change, flush, send backpressure
 * stretches). Single writer (BT thread); foreground readers snapshot under
 * IRQ-disable.
 *
 * Designed to live alongside the existing s_status_msg in bt-service: status
 * is a single-line "what's happening now"; this log is "what happened over
 * the last few seconds" so we can walk around with the device, induce a
 * cutout, then view what the controller was doing during the bad window.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BT_LINK_LOG_MSG_LEN  60

/* Drop the ring contents. Used on a fresh connection so the log shows
 * only events from the session being investigated. */
void bt_link_log_reset(void);

/* Logging is opt-in, toggled from the Bluetooth screen. While disabled,
 * bt_link_logf() and bt_link_log_dump() are no-ops, so the ring isn't
 * filled and the card isn't written during normal use. RAM-only state:
 * resets to OFF on every boot. Enabling clears the ring so the captured
 * window starts fresh. */
bool bt_link_log_enabled(void);
void bt_link_log_set_enabled(bool en);

/* Append a printf-formatted entry tagged with the current tick (ms).
 * Safe to call from BT thread; foreground callers must use bt_link_log_reset
 * /_get only. Truncates to BT_LINK_LOG_MSG_LEN-1 chars. */
void bt_link_logf(const char* fmt, ...) __attribute__((format(printf,1,2)));

/* Number of valid entries currently in the ring (<= ring capacity). */
int  bt_link_log_count(void);

/* Total entries ever logged this session — readers compare against count
 * to detect overflow / how many older entries were dropped. */
uint32_t bt_link_log_total(void);

/* Snapshot entry i where i=0 is the oldest currently in the ring and
 * i=count-1 is the newest. Returns false if i is out of range. */
bool bt_link_log_get(int i, uint32_t* ts_ms, char* out, size_t out_sz);

/* Flush new entries (since the last dump) to `path` as "SS.cc msg" lines,
 * oldest first. APPENDS rather than truncates, so multiple pauses — and
 * sessions across reboots — accumulate in one file instead of clobbering
 * each other. Each flush is preceded by a "--- pause ---" separator (or
 * "=== log start ===" for the first flush of a session / after a rotate),
 * and a "... N dropped ..." line if the ring overflowed between flushes.
 * The file self-limits: once it passes an internal size cap the next dump
 * rotates it. No-op while logging is disabled. BT-thread only (blocking
 * file I/O). Lets us read the log off the SD card instead of photographing
 * the screen. */
void bt_link_log_dump(const char* path);
