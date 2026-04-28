/***************************************************************************
 * BT A2DP — debug viewer.
 *
 * Thin foreground UI on top of bt-service. Lets the user enable BT, scan,
 * pick a device, connect/disconnect, and watch the service state — but
 * does not own the BT thread (the service does, and it survives leaving
 * this screen). This is the Phase A testbed; the proper Settings →
 * Bluetooth menu (Phase B) will replace it.
 *
 * Buttons:
 *   PLAY    — when there's a saved last device: connect to it.
 *             when scan results are showing: connect to highlighted entry.
 *   MENU    — start scan.
 *   PREV/NEXT (scroll wheel) — navigate scan-result list.
 *   BACK    — leave the screen (BT keeps running).
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#ifndef BOOTLOADER
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "action.h"
#include "kernel.h"

#include "bt-service.h"

static const char* state_str(enum bt_state s)
{
    switch(s) {
    case BT_STATE_OFF:        return "off";
    case BT_STATE_ENABLING:   return "enabling";
    case BT_STATE_READY:      return "ready";
    case BT_STATE_SCANNING:   return "scanning";
    case BT_STATE_CONNECTING: return "connecting";
    case BT_STATE_STREAMING:  return "STREAMING";
    case BT_STATE_FAILED:     return "FAILED";
    default:                   return "?";
    }
}

static int s_pick_sel;

static void format_dev(const struct bt_dev_info* d, int i, char* buf, size_t bufsz)
{
    char prefix = (i == s_pick_sel) ? '>' : ' ';
    if(d->name_set)
        snprintf(buf, bufsz, "%c %s", prefix, d->name);
    else
        snprintf(buf, bufsz, "%c %02X%02X%02X%02X%02X%02X", prefix,
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5]);
}

static void redraw(void)
{
    char ln[40];
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);

    enum bt_state st = bt_service_get_state();
    snprintf(ln, sizeof(ln), "BT: %s", state_str(st));
    lcd_puts(0, 0, ln);

    const char* msg = bt_service_get_status_msg();
    if(msg && msg[0]) lcd_puts(0, 1, msg);

    const char* conn = bt_service_get_connected_name();
    if(conn && conn[0]) {
        snprintf(ln, sizeof(ln), "→ %s", conn);
        lcd_puts(0, 2, ln);
    }

    if(st == BT_STATE_OFF) {
        lcd_puts(0, 4, "PLAY=enable");
        lcd_puts(0, 5, "BACK=exit");
    } else if(bt_service_have_last() && st == BT_STATE_READY) {
        const struct bt_dev_info* d = bt_service_get_last();
        snprintf(ln, sizeof(ln), "Last: %s",
                 d->name_set ? d->name : "(unknown)");
        lcd_puts(0, 4, ln);
        lcd_puts(0, 6, "PLAY=connect last");
        lcd_puts(0, 7, "MENU=scan");
        lcd_puts(0, 14, "BACK=leave (BT stays on)");
    }

    /* Scan-results panel — drawn whenever there are results, regardless of
     * state, so the user can pick after a completed scan. */
    struct bt_dev_info devs[BT_SERVICE_MAX_DEVS];
    int n = bt_service_get_scan_results(devs, BT_SERVICE_MAX_DEVS);
    if(n > 0) {
        snprintf(ln, sizeof(ln), "Devices (%d):", n);
        lcd_puts(0, 4, ln);
        for(int i = 0; i < n && i < 8; i++) {
            format_dev(&devs[i], i, ln, sizeof(ln));
            lcd_puts(0, 5 + i, ln);
        }
        lcd_puts(0, 14, "PLAY=pick MENU=rescan");
    }

    lcd_update();
}

bool dbg_bt_a2dp(void)
{
    s_pick_sel = 0;

    while(true) {
        redraw();
        int act = get_action(CONTEXT_STD, HZ/4);    /* 250 ms refresh */
        enum bt_state st = bt_service_get_state();

        if(act == ACTION_STD_CANCEL) break;

        if(act == ACTION_STD_OK) {
            if(st == BT_STATE_OFF) {
                bt_service_enable();
            } else if(st == BT_STATE_READY) {
                /* Prefer scan-results pick if there are any. */
                struct bt_dev_info devs[BT_SERVICE_MAX_DEVS];
                int n = bt_service_get_scan_results(devs, BT_SERVICE_MAX_DEVS);
                if(n > 0 && s_pick_sel < n) {
                    bt_service_connect_addr(devs[s_pick_sel].addr);
                } else if(bt_service_have_last()) {
                    bt_service_connect_last();
                }
            } else if(st == BT_STATE_STREAMING || st == BT_STATE_CONNECTING) {
                bt_service_disconnect();
            }
        } else if(act == ACTION_STD_MENU) {
            if(st == BT_STATE_READY)         bt_service_scan_start();
            else if(st == BT_STATE_SCANNING) bt_service_scan_stop();
        } else if(act == ACTION_STD_PREV || act == ACTION_STD_PREVREPEAT) {
            if(s_pick_sel > 0) s_pick_sel--;
        } else if(act == ACTION_STD_NEXT || act == ACTION_STD_NEXTREPEAT) {
            struct bt_dev_info tmp[BT_SERVICE_MAX_DEVS];
            int cur = bt_service_get_scan_results(tmp, BT_SERVICE_MAX_DEVS);
            if(s_pick_sel < cur - 1) s_pick_sel++;
        }
    }
    return false;
}
#endif /* BOOTLOADER */
