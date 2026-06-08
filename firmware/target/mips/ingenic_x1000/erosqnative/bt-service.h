/***************************************************************************
 * BT service — persistent A2DP source background service.
 *
 * Owns the BTstack run loop (in a dedicated kernel thread), the BT state
 * machine, and all BTstack interaction. UI screens consume this purely as
 * a control + status API; they never call BTstack directly.
 *
 * Lifecycle:
 *   bt_service_enable()   — power up chip, init stack, start BT thread.
 *                           State transitions OFF → ENABLING → READY.
 *   bt_service_disable()  — disconnect (if any), tear down stack, exit BT
 *                           thread, power down chip. Returns to OFF.
 *
 * Connection requests (scan/connect/disconnect) are command codes posted
 * from the calling thread to the BT thread; the BT thread processes them
 * inside its run loop. Status reads are non-blocking and lock-free (single
 * BT-thread writer, multi-thread readers; volatile + atomic word reads).
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BT_SERVICE_MAX_DEVS    12
#define BT_SERVICE_MAX_BONDED  8     /* matches NVM_NUM_LINK_KEYS in btstack_config.h */
#define BT_SERVICE_NAME_LEN    24
#define BT_SERVICE_STATUS_LEN  32

enum bt_state {
    BT_STATE_OFF = 0,       /* not enabled */
    BT_STATE_ENABLING,      /* hw power-on + patchram + stack init in progress */
    BT_STATE_READY,         /* enabled, no connection */
    BT_STATE_SCANNING,      /* HCI inquiry running */
    BT_STATE_CONNECTING,    /* L2CAP/AVDTP connect in progress */
    BT_STATE_STREAMING,     /* A2DP stream active, audio routed via BT */
    BT_STATE_FAILED,        /* last operation failed; status msg has details */
};

struct bt_dev_info {
    uint8_t addr[6];
    char    name[BT_SERVICE_NAME_LEN];
    int8_t  rssi;
    uint8_t name_set;
    uint8_t rssi_set;
};

/* ---- lifecycle ---- */

/* Power on the chip, bring up the stack, and start the BT thread. Blocks
 * until the chip is initialized or initialization fails. Returns 0 on
 * success, < 0 on failure (state will be BT_STATE_FAILED with details in
 * the status message). Idempotent — calling when already enabled returns 0. */
int  bt_service_enable(void);

/* Tear down: disconnect any active stream, exit the BT thread, power off
 * the chip. Idempotent. Blocks briefly during teardown. */
int  bt_service_disable(void);

bool bt_service_is_enabled(void);

/* Like bt_service_enable() but also auto-connects to the most-recently-
 * used bonded device once the stack reaches READY. Non-blocking: returns
 * as soon as the BT thread is spawned; the connect happens later inside
 * the BT thread's HCI_STATE_WORKING handler. Used by the boot
 * autoconnect path so playback can resume without UI interaction. */
int  bt_service_enable_and_connect_last(void);

/* ---- connection management (commands posted to BT thread) ---- */

int  bt_service_scan_start(void);    /* HCI inquiry, ~10s. Use bt_service_get_scan_results to poll. */
int  bt_service_scan_stop(void);
int  bt_service_connect_addr(const uint8_t addr[6]);
int  bt_service_connect_last(void);  /* convenience: connect to bonded[0] */
int  bt_service_disconnect(void);

/* Forget (un-pair) a previously-bonded device: drops it from the
 * persistent bonded-devices list AND deletes its link key from BTstack's
 * link-key DB so the speaker won't auto-reconnect. If the device is the
 * one currently active, also disconnects. Posted to the BT thread. */
int  bt_service_forget(const uint8_t addr[6]);

/* ---- status / introspection (lock-free, polled by UI) ---- */

enum bt_state bt_service_get_state(void);
/* True whenever an HCI inquiry is in progress, regardless of whether
 * the connection state is READY/CONNECTING/STREAMING. Lets the UI show
 * "scanning..." while keeping the streaming-device indicator visible. */
bool          bt_service_is_scanning(void);
/* Latest one-line status message — survives across state changes for UI. */
const char*   bt_service_get_status_msg(void);
/* Human-readable name of currently-connected device, "" if none. */
const char*   bt_service_get_connected_name(void);
/* Copy the address of the device currently being streamed to / connected to
 * into out[6] and return true; false (out untouched) if idle. Lets the UI
 * tell "the selected paired device" from "the active one" so a tap can switch
 * vs. disconnect. */
bool          bt_service_get_active_addr(uint8_t out[6]);
/* Snapshot of current scan results into caller's buffer. */
int           bt_service_get_scan_results(struct bt_dev_info* out, int max);
/* Discard accumulated scan results (no-op while a scan is in progress).
 * The UI calls this on entry so it opens on the paired list instead of stale
 * results from a previous visit, which otherwise hide the paired list. */
void          bt_service_scan_clear(void);
/* Snapshot of bonded (paired and remembered) devices, ordered most-recently
 * connected first. Returns the number copied. */
int           bt_service_get_bonded(struct bt_dev_info* out, int max);
/* True if there is at least one bonded device on file. */
bool          bt_service_have_last(void);
/* Pointer to in-memory copy of the most-recently-connected bonded device.
 * Valid only while bt service is loaded. NULL if none. */
const struct bt_dev_info* bt_service_get_last(void);
