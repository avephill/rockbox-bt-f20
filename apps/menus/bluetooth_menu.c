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
#include "lang.h"
#include "option_select.h"

#include "bt-service.h"

#if !defined(BOOTLOADER) && CONFIG_CPU == X1000
#include "crash_log.h"
#define BTM_BC(...)  crash_log_breadcrumbf(__VA_ARGS__)
#else
#define BTM_BC(...)  do { } while (0)
#endif

/* Define to show the raw bt-service status string (a2dp rc=…, sel SBC rc=…,
 * rej XXXX, avrcp op=…) on the screen. Off for release — it's developer
 * jargon; the BT Link Log screen is the real diagnostic surface. */
/* #define BT_MENU_DEBUG */

static const char* state_str(enum bt_state s)
{
    switch(s) {
    case BT_STATE_OFF:        return "off";
    case BT_STATE_ENABLING:   return "turning on...";
    case BT_STATE_READY:      return "on";
    case BT_STATE_SCANNING:   return "searching...";
    case BT_STATE_CONNECTING: return "connecting...";
    case BT_STATE_STREAMING:  return "connected";
    case BT_STATE_FAILED:     return "connection failed";
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
                       bool is_active, char* buf, size_t bufsz)
{
    char prefix = (i == sel) ? '>' : ' ';
    const char* tag = is_active ? " (on)" : "";   /* mark the connected device */
    if(d->name_set)
        snprintf(buf, bufsz, "%c %s%s", prefix, d->name, tag);
    else
        snprintf(buf, bufsz, "%c %02X%02X%02X%02X%02X%02X%s", prefix,
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5], tag);
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

/* Result of the device actions menu (below). */
enum bt_menu_action { BT_MENU_CANCEL, BT_MENU_SCAN, BT_MENU_FORGET };

/* Explicit actions menu, opened by pressing MENU on the device list.
 *
 * This replaces the old "long-press MENU to forget" gesture: tap and hold
 * on the same button (MENU) were only distinguished by timing, so a slightly
 * short hold fired a scan instead of a forget. Now MENU always opens this
 * labelled, navigable menu and Forget is an unambiguous entry.
 *
 * `sel` is the highlighted bonded device, or NULL when none is selectable
 * (e.g. the scan-results view) — in which case Forget is not offered.
 * Returns the chosen action; BT_MENU_CANCEL if the user backs out. */
static enum bt_menu_action bt_actions_menu(const struct bt_dev_info* sel,
                                           bool scanning)
{
    const char*          labels[2];
    enum bt_menu_action  acts[2];
    int n = 0;
    labels[n] = scanning ? "Stop scanning" : "Scan for new devices";
    acts[n++] = BT_MENU_SCAN;
    if(sel) {
        labels[n] = "Forget this device";
        acts[n++] = BT_MENU_FORGET;
    }

    int cur = 0;
    while(true) {
        FOR_NB_SCREENS(s) {
            struct screen* screen = &screens[s];
            struct viewport vp;
            memset(&vp, 0, sizeof(vp));
            viewport_set_defaults(&vp, s);
            vp.font = screen->getuifont();
            struct viewport* last = screen->set_viewport(&vp);
            screen->clear_viewport();

            char ln[48];
            int line = 0;
            int nb_lines = viewport_get_nb_lines(&vp);
            if(sel && sel->name_set)
                snprintf(ln, sizeof(ln), "%s", sel->name);
            else
                snprintf(ln, sizeof(ln), "Bluetooth");
            screen->puts_scroll(0, line++, ln);
            if(line < nb_lines) line++;         /* spacer */
            for(int i = 0; i < n && line < nb_lines - 1; i++) {
                snprintf(ln, sizeof(ln), "%c %s", i == cur ? '>' : ' ',
                         labels[i]);
                screen->puts_scroll(0, line++, ln);
            }
            if(nb_lines - 1 > line)
                screen->puts_scroll(0, nb_lines - 1, "PLAY=select  BACK=cancel");
            screen->update_viewport();
            screen->set_viewport(last);
        }

        int act = get_action(CONTEXT_STD, TIMEOUT_BLOCK);
        switch(act) {
        case ACTION_STD_PREV: case ACTION_STD_PREVREPEAT:
            if(cur > 0) cur--;
            break;
        case ACTION_STD_NEXT: case ACTION_STD_NEXTREPEAT:
            if(cur < n - 1) cur++;
            break;
        case ACTION_STD_OK:
            return acts[cur];
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:               /* MENU again closes the menu */
            return BT_MENU_CANCEL;
        default:
            break;
        }
    }
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

    /* This screen is purely the device list now. Power off, audio quality and
     * link logging live as their own entries in the parent Bluetooth menu. */
    int  n_sel = n_list;

    /* Clamp the cursor whenever the selectable count shrinks (e.g. a forget
     * removed a bonded entry, or a fresh scan started). */
    if(s_pick_sel >= n_sel) s_pick_sel = n_sel > 0 ? n_sel - 1 : 0;
    if(s_pick_sel < 0)      s_pick_sel = 0;

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

#ifdef BT_MENU_DEBUG
        const char* msg = bt_service_get_status_msg();
        if(msg && msg[0]) screen->puts_scroll(0, line++, msg);
#endif

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
            /* reserve a line for the hint */
            int max_devs = nb_lines - line - 2;
            if(max_devs > n_list) max_devs = n_list;
            uint8_t active[6];
            bool have_active = bt_service_get_active_addr(active);
            for(int i = 0; i < max_devs; i++) {
                bool is_active = have_active
                               && memcmp(active, list[i].addr, 6) == 0;
                format_dev(&list[i], i, s_pick_sel, is_active, ln, sizeof(ln));
                screen->puts_scroll(0, line++, ln);
            }
        }

        /* Bottom hint line. MENU opens the actions menu (scan / forget) — a
         * single reliable tap, no more long-press. PLAY is the one-tap
         * connect/switch/disconnect action. */
        int last_line = nb_lines - 1;
        const char* hint = "";
        if(bt_service_is_scanning()) {
            hint = list_is_scan && n_list > 0
                     ? "PLAY=switch  MENU=options"
                     : "MENU=options";
        } else {
            switch(st) {
            case BT_STATE_OFF:        hint = "PLAY=on  BACK=exit"; break;
            case BT_STATE_READY:
                if(list_is_scan)      hint = "PLAY=connect  MENU=options";
                else if(n_list > 0)   hint = "PLAY=connect  MENU=options";
                else                  hint = "MENU=scan";
                break;
            case BT_STATE_SCANNING:   hint = "MENU=options"; break;
            case BT_STATE_STREAMING:
                hint = "PLAY=switch  MENU=options";
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
    BTM_BC("bt-menu: opened");

    /* Auto-enable BT on entry. Idempotent if already on. */
    bt_service_enable();
    /* Open on the paired list: drop any scan results left over from a previous
     * visit (they otherwise hide the paired devices, making it impossible to
     * pick/switch a remembered device without rescanning). MENU rescans. */
    bt_service_scan_clear();

    while(true) {
        redraw();
        int act = get_action(CONTEXT_STD, HZ/4);
        if(act != ACTION_NONE && act != ACTION_UNKNOWN)
            BTM_BC("bt-menu: action %d sel=%d", act, s_pick_sel);
        if(act == ACTION_STD_CANCEL) break;

        enum bt_state st = bt_service_get_state();
        struct bt_dev_info list[BT_SERVICE_MAX_DEVS];
        bool list_is_scan = false;
        int n_list = active_list(list, BT_SERVICE_MAX_DEVS, &list_is_scan);
        int  n_sel = n_list;

        if(act == ACTION_STD_OK) {
            if(st == BT_STATE_OFF) {
                bt_service_enable();
            } else if(n_list > 0 && s_pick_sel < n_list) {
                /* A device is selected (scan result or paired). Tapping a
                 * device that is NOT the active one connects to it — or, if
                 * something is already streaming, switches to it in one press
                 * (bt-service does the disconnect-then-connect handoff).
                 * Tapping the device that IS currently active disconnects it.
                 * This makes the paired list behave like the scan list, so the
                 * user never has to "disconnect, then reconnect" to switch. */
                uint8_t active[6];
                bool have_active = bt_service_get_active_addr(active);
                if(have_active && memcmp(active, list[s_pick_sel].addr, 6) == 0)
                    bt_service_disconnect();
                else
                    bt_service_connect_addr(list[s_pick_sel].addr);
            } else if(st == BT_STATE_STREAMING || st == BT_STATE_CONNECTING) {
                /* No list to tap, but something is up — PLAY disconnects it. */
                bt_service_disconnect();
            } else if(st == BT_STATE_FAILED) {
                /* On retry, re-enable to flush state. */
                bt_service_disable();
                bt_service_enable();
            }
        } else if(act == ACTION_STD_MENU || act == ACTION_STD_CONTEXT) {
            /* MENU (tap or hold) opens the actions menu: scan for new
             * devices, and — when a bonded device is highlighted — forget
             * it. Forget used to be a MENU long-press, but tap-vs-hold on
             * the same button made it easy to trigger a scan by mistake, so
             * it is now an explicit, labelled menu entry. Works during
             * STREAMING: a scan shares the radio's inquiry windows with
             * audio, so playback may glitch briefly, but the user asked. */
            if(st != BT_STATE_OFF && st != BT_STATE_ENABLING) {
                bool can_forget = !list_is_scan && n_list > 0
                                  && s_pick_sel < n_list;
                /* Snapshot the selection: bt_service_forget / a scan can
                 * reshape the underlying list, and we use it after the menu. */
                struct bt_dev_info seldev;
                if(can_forget) seldev = list[s_pick_sel];
                switch(bt_actions_menu(can_forget ? &seldev : NULL,
                                       bt_service_is_scanning())) {
                case BT_MENU_SCAN:
                    if(bt_service_is_scanning()) bt_service_scan_stop();
                    else                          bt_service_scan_start();
                    break;
                case BT_MENU_FORGET:
                    if(can_forget && confirm_forget(&seldev)) {
                        bt_service_forget(seldev.addr);
                        splashf(HZ, "Forgot %s",
                                seldev.name_set ? seldev.name : "device");
                    }
                    break;
                case BT_MENU_CANCEL:
                default:
                    break;
                }
            }
        } else if(act == ACTION_STD_PREV || act == ACTION_STD_PREVREPEAT) {
            if(s_pick_sel > 0) s_pick_sel--;
        } else if(act == ACTION_STD_NEXT || act == ACTION_STD_NEXTREPEAT) {
            if(s_pick_sel < n_sel - 1) s_pick_sel++;
        }
    }
    return 0;
}

/* --- parent "Bluetooth" menu items ----------------------------------------
 * Audio quality, Link logging and Auto-connect are shown with their current
 * value inline ("Audio quality: 128 kbps") via DYNTEXT items: a text callback
 * renders "name: value", and selecting one opens the normal Rockbox option
 * chooser through option_screen(), so persistence and the link-logging change
 * callback (settings_list.c) still run. */

static void bt_edit_setting(const void *var)
{
    const struct settings_list *s = find_setting(var);
    if(s)
        option_screen(s, NULL, s->flags & F_TEMPVAR, str(s->lang_id));
}

static int bt_aac_bitrate_edit(void)
{
    bt_edit_setting(&global_settings.bt_aac_bitrate);
    return 0;
}
static char* bt_aac_bitrate_text(int sel, void *data, char *buf, size_t len)
{
    (void)sel; (void)data;
    const char* q = global_settings.bt_aac_bitrate == 2 ? "64 kbps"
                  : global_settings.bt_aac_bitrate == 1 ? "96 kbps"
                  : "128 kbps";
    snprintf(buf, len, "%s: %s", (char*)str(LANG_BT_AUDIO_QUALITY), q);
    return buf;
}

static int bt_link_logging_edit(void)
{
    bt_edit_setting(&global_settings.bt_link_logging);
    return 0;
}
static char* bt_link_logging_text(int sel, void *data, char *buf, size_t len)
{
    (void)sel; (void)data;
    snprintf(buf, len, "%s: %s", (char*)str(LANG_BT_LINK_LOGGING),
             global_settings.bt_link_logging ? "On" : "Off");
    return buf;
}

static int bt_autoconnect_edit(void)
{
    bt_edit_setting(&global_settings.bt_autoconnect);
    return 0;
}
static char* bt_autoconnect_text(int sel, void *data, char *buf, size_t len)
{
    (void)sel; (void)data;
    snprintf(buf, len, "%s: %s", (char*)str(LANG_BT_AUTOCONNECT_ON_BOOT),
             global_settings.bt_autoconnect ? "On" : "Off");
    return buf;
}

static int bt_turn_off(void)
{
    if(bt_service_is_enabled()) {
        bt_service_disable();
        splashf(HZ, "Bluetooth off");
    } else {
        splashf(HZ, "Bluetooth is off");
    }
    return 0;
}

MENUITEM_FUNCTION(bt_open_screen_item, 0, "Devices",
                  bt_open_screen, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(bt_aac_bitrate_item, 0, bt_aac_bitrate_edit,
                          bt_aac_bitrate_text, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(bt_link_logging_item, 0, bt_link_logging_edit,
                          bt_link_logging_text, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(bt_autoconnect_item, 0, bt_autoconnect_edit,
                          bt_autoconnect_text, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION(bt_turn_off_item, 0, "Turn Bluetooth off",
                  bt_turn_off, NULL, Icon_NOICON);

MAKE_MENU(bluetooth_menu, "Bluetooth", NULL, Icon_NOICON,
          &bt_open_screen_item, &bt_aac_bitrate_item,
          &bt_link_logging_item, &bt_autoconnect_item,
          &bt_turn_off_item);

#endif /* HAVE_BT_PCM_SINK */
