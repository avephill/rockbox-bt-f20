/***************************************************************************
 * Settings → Bluetooth menu (Phase B of roadmap C8).
 *
 * Single interactive screen on top of bt-service. Auto-enables the BT
 * service on entry, lets the user reconnect to the saved last device or
 * scan-and-pair, and shows live status. Leaving the screen leaves the BT
 * service running so audio keeps streaming while the user navigates the
 * rest of Rockbox.
 *
 * Phase C will add a persisted "Bluetooth enabled" setting and an
 * autoconnect-on-boot hook so opening a track Just Plays through BT
 * without ever entering this menu.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#include "config.h"

#ifdef HAVE_BT_PCM_SINK

#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "lcd.h"
#include "font.h"
#include "action.h"
#include "kernel.h"
#include "system.h"
#include "settings.h"
#include "viewport.h"
#include "screen_access.h"
#include "yesno.h"
#include "splash.h"

#include "bt-service.h"

static const char* state_str(enum bt_state s)
{
    switch(s) {
    case BT_STATE_OFF:        return "off";
    case BT_STATE_ENABLING:   return "enabling...";
    case BT_STATE_READY:      return "ready";
    case BT_STATE_SCANNING:   return "scanning...";
    case BT_STATE_CONNECTING: return "connecting...";
    case BT_STATE_STREAMING:  return "STREAMING";
    case BT_STATE_FAILED:     return "FAILED";
    default:                  return "?";
    }
}

/* Cursor index into whichever list is currently shown:
 *   - scan results when n_scan > 0
 *   - bonded devices otherwise (when state==READY)
 * Re-clamped each redraw because the underlying list size can change as
 * the BT thread reports inquiry results or the user forgets entries. */
static int s_pick_sel;

static void format_dev(const struct bt_dev_info* d, int i, int sel,
                       char* buf, size_t bufsz)
{
    char prefix = (i == sel) ? '>' : ' ';
    if(d->name_set)
        snprintf(buf, bufsz, "%c %s", prefix, d->name);
    else
        snprintf(buf, bufsz, "%c %02X%02X%02X%02X%02X%02X", prefix,
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5]);
}

/* Snapshot the list the cursor currently navigates. Scan results take
 * precedence whenever a scan is in progress *or* has produced results
 * since the last view-reset, so the user can pick a freshly-discovered
 * speaker even while a previous one is still streaming. Otherwise we
 * fall back to the bonded list. */
static int active_list(struct bt_dev_info* out, int max,
                       bool* out_is_scan)
{
    int n = bt_service_get_scan_results(out, max);
    if(n > 0 || bt_service_is_scanning()) {
        if(out_is_scan) *out_is_scan = true;
        return n;
    }
    if(out_is_scan) *out_is_scan = false;
    return bt_service_get_bonded(out, max);
}

static bool confirm_forget(const struct bt_dev_info* d)
{
    char title[40];
    char addr[20];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->addr[0], d->addr[1], d->addr[2],
             d->addr[3], d->addr[4], d->addr[5]);
    snprintf(title, sizeof(title), "Forget %s?",
             d->name_set ? d->name : addr);
    const char *lines[1] = { title };
    const struct text_message prompt = { lines, 1 };
    return gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES;
}

/* Render into the theme's content viewport so the status bar / theme
 * frame stays visible (this is what ordinary Rockbox screens do). */
static void redraw(void)
{
    char ln[48];
    enum bt_state st = bt_service_get_state();
    struct bt_dev_info list[BT_SERVICE_MAX_DEVS];
    bool list_is_scan = false;
    int n_list = active_list(list, BT_SERVICE_MAX_DEVS, &list_is_scan);

    /* Clamp the cursor whenever the underlying list shrinks (e.g. a
     * forget removed a bonded entry, or a fresh scan started). */
    if(s_pick_sel >= n_list) s_pick_sel = n_list > 0 ? n_list - 1 : 0;
    if(s_pick_sel < 0)       s_pick_sel = 0;

    FOR_NB_SCREENS(s) {
        struct screen* screen = &screens[s];
        struct viewport vp;
        /* viewport_set_defaults selects the SBS info viewport when a theme
         * is active, full-screen otherwise — same convention as ordinary
         * Rockbox screens, so the theme's status bar and frame remain
         * visible. Zero before init: init_viewport reads vp.buffer first,
         * and uninitialized stack garbage there crashes inside lcd. */
        memset(&vp, 0, sizeof(vp));
        viewport_set_defaults(&vp, s);
        vp.font = screen->getuifont();
        struct viewport* last = screen->set_viewport(&vp);

        screen->clear_viewport();

        int line = 0;
        int nb_lines = viewport_get_nb_lines(&vp);

        /* Status line: state, plus a "+ scanning" suffix when an inquiry
         * is running concurrently with a stream (so the user knows MENU
         * is doing something even though state still says STREAMING). */
        if(bt_service_is_scanning() && st != BT_STATE_SCANNING)
            snprintf(ln, sizeof(ln), "Bluetooth: %s + scanning", state_str(st));
        else
            snprintf(ln, sizeof(ln), "Bluetooth: %s", state_str(st));
        screen->puts_scroll(0, line++, ln);

        const char* msg = bt_service_get_status_msg();
        if(msg && msg[0]) screen->puts_scroll(0, line++, msg);

        const char* conn = bt_service_get_connected_name();
        if(conn && conn[0]) {
            snprintf(ln, sizeof(ln), "On: %s", conn);
            screen->puts_scroll(0, line++, ln);
        }

        if(line < nb_lines) line++;     /* spacer */

        if(n_list > 0 && line < nb_lines - 1) {
            snprintf(ln, sizeof(ln), "%s (%d):",
                     list_is_scan ? "Found" : "Paired", n_list);
            screen->puts_scroll(0, line++, ln);
            int max_devs = nb_lines - line - 2;
            if(max_devs > n_list) max_devs = n_list;
            for(int i = 0; i < max_devs; i++) {
                format_dev(&list[i], i, s_pick_sel, ln, sizeof(ln));
                screen->puts_scroll(0, line++, ln);
            }
        }

        /* Bottom hint line. The bonded-list hint advertises MENU long-press
         * because that's the action our keymap binds to ACTION_STD_CONTEXT
         * (BUTTON_MENU|BUTTON_REPEAT) — long-pressing PLAY fires HOTKEY,
         * which we deliberately don't intercept. The streaming hint also
         * mentions MENU=add since scan now works during streaming. */
        int last_line = nb_lines - 1;
        const char* hint = "";
        if(bt_service_is_scanning()) {
            hint = list_is_scan && n_list > 0
                     ? "PLAY=switch  MENU=stop scan"
                     : "MENU=stop scan";
        } else {
            switch(st) {
            case BT_STATE_OFF:        hint = "PLAY=on  BACK=exit"; break;
            case BT_STATE_READY:
                if(list_is_scan)      hint = "PLAY=connect  MENU=rescan";
                else if(n_list > 0)   hint = "PLAY=play  hold MENU=forget";
                else                  hint = "MENU=scan";
                break;
            case BT_STATE_SCANNING:   hint = "MENU=stop scan"; break;
            case BT_STATE_STREAMING:
                hint = list_is_scan
                         ? "PLAY=switch  MENU=rescan"
                         : "PLAY=disconnect  MENU=add device";
                break;
            case BT_STATE_CONNECTING: hint = "PLAY=disconnect"; break;
            case BT_STATE_FAILED:     hint = "MENU=retry"; break;
            default: break;
            }
        }
        if(last_line > line)
            screen->puts_scroll(0, last_line, hint);

        screen->update_viewport();
        screen->set_viewport(last);
    }
}

int bt_open_screen(void);
int bt_open_screen(void)
{
    s_pick_sel = 0;

    /* Auto-enable BT on entry. Idempotent if already on. */
    bt_service_enable();

    while(true) {
        redraw();
        int act = get_action(CONTEXT_STD, HZ/4);
        if(act == ACTION_STD_CANCEL) break;

        enum bt_state st = bt_service_get_state();
        struct bt_dev_info list[BT_SERVICE_MAX_DEVS];
        bool list_is_scan = false;
        int n_list = active_list(list, BT_SERVICE_MAX_DEVS, &list_is_scan);

        if(act == ACTION_STD_OK) {
            if(st == BT_STATE_OFF) {
                bt_service_enable();
            } else if(list_is_scan && n_list > 0 && s_pick_sel < n_list) {
                /* Scan-result picked: connect (or switch). bt-service
                 * handles the disconnect-then-connect handoff if a
                 * stream is currently up. */
                bt_service_connect_addr(list[s_pick_sel].addr);
            } else if(st == BT_STATE_READY) {
                if(n_list > 0 && s_pick_sel < n_list)
                    bt_service_connect_addr(list[s_pick_sel].addr);
            } else if(st == BT_STATE_STREAMING || st == BT_STATE_CONNECTING) {
                bt_service_disconnect();
            } else if(st == BT_STATE_FAILED) {
                /* On retry, re-enable to flush state. */
                bt_service_disable();
                bt_service_enable();
            }
        } else if(act == ACTION_STD_CONTEXT) {
            /* Long-press OK on a bonded device → forget it (with confirm).
             * Only meaningful when the bonded list is the active one;
             * forgetting a scan-result that's not yet bonded is a no-op. */
            if(!list_is_scan && st == BT_STATE_READY
               && n_list > 0 && s_pick_sel < n_list) {
                if(confirm_forget(&list[s_pick_sel])) {
                    bt_service_forget(list[s_pick_sel].addr);
                    splashf(HZ, "Forgot %s",
                            list[s_pick_sel].name_set
                              ? list[s_pick_sel].name : "device");
                }
            }
        } else if(act == ACTION_STD_MENU) {
            /* MENU is "scan / add new device" everywhere except OFF.
             * If a scan is already running it stops it; otherwise
             * starts one. Works during STREAMING — the radio shares
             * inquiry windows with audio, so playback may glitch
             * briefly but the user explicitly asked to look. */
            if(st != BT_STATE_OFF && st != BT_STATE_ENABLING) {
                if(bt_service_is_scanning()) bt_service_scan_stop();
                else                          bt_service_scan_start();
            }
        } else if(act == ACTION_STD_PREV || act == ACTION_STD_PREVREPEAT) {
            if(s_pick_sel > 0) s_pick_sel--;
        } else if(act == ACTION_STD_NEXT || act == ACTION_STD_NEXTREPEAT) {
            if(s_pick_sel < n_list - 1) s_pick_sel++;
        }
    }
    return 0;
}

MENUITEM_FUNCTION(bt_open_screen_item, 0, "Open Bluetooth",
                  bt_open_screen, NULL, Icon_NOICON);
MENUITEM_SETTING(bt_autoconnect_item, &global_settings.bt_autoconnect, NULL);

MAKE_MENU(bluetooth_menu, "Bluetooth", NULL, Icon_NOICON,
          &bt_open_screen_item, &bt_autoconnect_item);

#endif /* HAVE_BT_PCM_SINK */
