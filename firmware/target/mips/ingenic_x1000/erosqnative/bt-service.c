/***************************************************************************
 * BT service — see bt-service.h.
 *
 * Replaces the old debug-screen-driven flow in bt-a2dp.c with a persistent
 * background service. The BT thread runs whenever BT is enabled and is
 * decoupled from any UI screen — audio keeps streaming while the user
 * navigates Rockbox.
 *
 * Threading contract:
 *   - All BTstack calls (HCI, L2CAP, AVDTP, A2DP, GAP, sink swap) happen
 *     on the BT thread. Foreground/UI threads only post commands and read
 *     status fields.
 *   - Commands are encoded as a single pending command word + bd_addr
 *     argument buffer; the BT thread polls these once per run-loop
 *     iteration. This is enough because the UI never enqueues commands
 *     faster than the BT thread can process them.
 *   - Status fields (s_state, s_status_msg, s_connected_name, s_devs)
 *     have a single writer (BT thread) and many readers (UI). Word reads
 *     are atomic on MIPS; the buffers are sized so torn reads are bounded
 *     to a single in-flight value, not a free.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#ifndef BOOTLOADER

#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "rbpaths.h"

#include "bt-erosqnative.h"
#include "bt-bcm-patchram.h"
#include "bt-btstack-hal.h"
#include "bt-tlv.h"

#include "hci.h"
#include "hci_cmd.h"
#include "gap.h"
#include "btstack_memory.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "hci_transport.h"
#include "hci_transport_h4.h"
#include "btstack_uart.h"
#include "hal_uart_dma.h"

#include "classic/btstack_link_key_db_tlv.h"
#include "classic/sdp_server.h"
#include "classic/a2dp.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/avdtp_util.h"
#include "classic/avrcp.h"
#include "classic/avrcp_target.h"
#include "classic/avrcp_controller.h"
#include "l2cap.h"

#include "pcm.h"
#include "pcm_sink.h"
#include "bt-pcm-sink.h"
#include "audio.h"

#include "bt-service.h"
#include "bt-link-log.h"
#include "crash_log.h"

extern const btstack_uart_t * btstack_uart_block_embedded_instance(void);

/* ---- thread + sync ---- */

#define BT_THREAD_STACK_BYTES   (DEFAULT_STACK_SIZE + 0x2000)
static long           s_stack[BT_THREAD_STACK_BYTES/sizeof(long)];
static unsigned int   s_thread_id;
static const char     s_thread_name[] = "bt_service";
static volatile bool  s_thread_running;

/* Commands posted by foreground → consumed by BT thread. */
enum bt_cmd {
    BT_CMD_NONE = 0,
    BT_CMD_DISABLE,
    BT_CMD_SCAN_START,
    BT_CMD_SCAN_STOP,
    BT_CMD_CONNECT_LAST,
    BT_CMD_CONNECT_ADDR,
    BT_CMD_DISCONNECT,
    BT_CMD_FORGET,
};
static volatile int   s_cmd;
static volatile bool  s_cmd_pending;
static bd_addr_t      s_cmd_addr;        /* arg for CONNECT_ADDR / FORGET */

/* ---- visible status (single writer = BT thread; readers anywhere) ---- */

static volatile enum bt_state s_state = BT_STATE_OFF;
static char                   s_status_msg[BT_SERVICE_STATUS_LEN];
static char                   s_connected_name[BT_SERVICE_NAME_LEN];

static struct bt_dev_info     s_devs[BT_SERVICE_MAX_DEVS];
static int                    s_n_devs;

/* Persistent bonded-devices list. s_bonded[0] is most-recently-connected
 * (so connect_last and the UI's "default device" both naturally pick it).
 * Keeping the on-disk ordering matches BTstack's TLV link_key_db, which
 * is also LRU-ordered via a sequence number embedded in each value. */
static struct bt_dev_info     s_bonded[BT_SERVICE_MAX_BONDED];
static int                    s_n_bonded;
static bool                   s_saved_this_session;

/* When true, the BT thread auto-issues a connect_last as soon as HCI
 * reaches the WORKING state. Set by bt_service_enable_and_connect_last
 * (the boot-autoconnect entry point), cleared once consumed so manual
 * UI flows aren't surprised by an unexpected reconnect. */
static volatile bool          s_autoconnect_on_ready;

/* Scanning is orthogonal to the connection state — the user can search
 * for a new speaker while one is already streaming (Bluetooth Classic
 * shares the radio, so audio may glitch briefly during inquiry windows,
 * but the alternative — forcing the user to disconnect first just to
 * pair a second device — is worse UX). The state enum still has
 * BT_STATE_SCANNING for the "idle and scanning" case to preserve the
 * existing UI hints; this flag covers "streaming and also scanning". */
static volatile bool          s_scanning;

/* Address pending a connect-after-disconnect handoff. Used by the UI
 * "tap a scan result while streaming → switch device" flow: we kick off
 * a disconnect, stash the new address here, and the STREAM_RELEASED
 * handler picks it up to issue the new connect once the radio is free. */
static bd_addr_t              s_pending_switch_addr;
static bool                   s_pending_switch;

/* ---- BT-thread-local state ---- */

static uint16_t s_a2dp_cid;
static uint8_t  s_local_seid;
static uint16_t s_avrcp_cid;
static bool     s_picked_valid;
static struct bt_dev_info s_picked;

/* SBC capabilities: 44.1 kHz stereo, all block/subband modes.
 * max_bitpool capped at 35 (not the typical 53) — Apple H1-class sinks
 * (AirPods/Beats) glitch on SBC at high bitpool; lower cap = smaller
 * on-air frames = more retransmit headroom. ~240 kbps is still transparent. */
static uint8_t sbc_caps[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF, 2, 35,
};
static uint8_t sbc_config[4];

/* ---- bonded-devices persistence (bt_bonded.dat, magic BTB1) ----
 *
 * Multi-device replacement for the old single-device bt_last.dat. The
 * file stores up to BT_SERVICE_MAX_BONDED entries in MRU order — index 0
 * is the most recently connected, so connect_last just picks bonded[0].
 *
 * BTstack's link_key_db_tlv is the source of truth for the link keys
 * themselves; this file holds the human-readable name and ordering
 * metadata. forget() drops both halves so the speaker can't auto-reconnect.
 */

#define BT_BONDED_PATH      ROCKBOX_DIR "/bt_bonded.dat"
#define BT_BONDED_PATH_TMP  ROCKBOX_DIR "/bt_bonded.tmp"
#define BT_BONDED_MAGIC     0x42544231u   /* "BTB1" */

#define BT_LAST_PATH        ROCKBOX_DIR "/bt_last.dat"   /* legacy; migrated on load */
#define BT_LAST_MAGIC       0x42544C31u

struct bt_bonded_entry {
    uint8_t addr[6];
    uint8_t name_set;
    uint8_t pad;
    char    name[BT_SERVICE_NAME_LEN];
};

struct bt_bonded_file {
    uint32_t magic;
    uint32_t count;          /* number of valid entries (0..BT_SERVICE_MAX_BONDED) */
    struct bt_bonded_entry entries[BT_SERVICE_MAX_BONDED];
};

struct bt_last_file {
    uint32_t magic;
    uint8_t  addr[6];
    uint8_t  name_set;
    uint8_t  pad;
    char     name[BT_SERVICE_NAME_LEN];
};

static void copy_entry_to_dev(struct bt_dev_info* d, const struct bt_bonded_entry* e)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, e->addr, 6);
    d->name_set = e->name_set;
    if(e->name_set) {
        size_t cp = sizeof(d->name) - 1;
        if(cp > sizeof(e->name)) cp = sizeof(e->name);
        memcpy(d->name, e->name, cp);
        d->name[cp] = '\0';
    }
}

static void copy_dev_to_entry(struct bt_bonded_entry* e, const struct bt_dev_info* d)
{
    memset(e, 0, sizeof(*e));
    memcpy(e->addr, d->addr, 6);
    e->name_set = d->name_set;
    if(d->name_set) {
        size_t cp = strlen(d->name);
        if(cp > sizeof(e->name)) cp = sizeof(e->name);
        memcpy(e->name, d->name, cp);
    }
}

static void bonded_save(void)
{
    struct bt_bonded_file f;
    memset(&f, 0, sizeof(f));
    f.magic = BT_BONDED_MAGIC;
    f.count = (uint32_t)s_n_bonded;
    for(int i = 0; i < s_n_bonded; i++)
        copy_dev_to_entry(&f.entries[i], &s_bonded[i]);

    int fd = open(BT_BONDED_PATH_TMP, O_CREAT|O_WRONLY|O_TRUNC, 0666);
    if(fd < 0) return;
    int n = write(fd, &f, sizeof(f));
    close(fd);
    if(n != (int)sizeof(f)) {
        remove(BT_BONDED_PATH_TMP);
        return;
    }
    remove(BT_BONDED_PATH);
    rename(BT_BONDED_PATH_TMP, BT_BONDED_PATH);
}

static bool bonded_load_v1(void)
{
    int fd = open(BT_BONDED_PATH, O_RDONLY);
    if(fd < 0) return false;
    struct bt_bonded_file f;
    int n = read(fd, &f, sizeof(f));
    close(fd);
    if(n != (int)sizeof(f) || f.magic != BT_BONDED_MAGIC) return false;
    if(f.count > BT_SERVICE_MAX_BONDED) f.count = BT_SERVICE_MAX_BONDED;
    s_n_bonded = (int)f.count;
    for(int i = 0; i < s_n_bonded; i++)
        copy_entry_to_dev(&s_bonded[i], &f.entries[i]);
    return true;
}

/* If the new bonded file isn't present but the legacy single-device file
 * is, migrate so the user doesn't lose their pairing across this upgrade. */
static bool bonded_migrate_from_last(void)
{
    int fd = open(BT_LAST_PATH, O_RDONLY);
    if(fd < 0) return false;
    struct bt_last_file lf;
    int n = read(fd, &lf, sizeof(lf));
    close(fd);
    if(n != (int)sizeof(lf) || lf.magic != BT_LAST_MAGIC) return false;

    struct bt_dev_info d;
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, lf.addr, 6);
    d.name_set = lf.name_set;
    if(lf.name_set) {
        size_t cp = sizeof(d.name) - 1;
        if(cp > sizeof(lf.name)) cp = sizeof(lf.name);
        memcpy(d.name, lf.name, cp);
        d.name[cp] = '\0';
    }
    s_n_bonded = 1;
    s_bonded[0] = d;
    bonded_save();
    /* Leave bt_last.dat in place for one boot in case the user rolls back;
     * not worth the complexity of cleanup. */
    return true;
}

static void bonded_load(void)
{
    s_n_bonded = 0;
    if(bonded_load_v1()) return;
    bonded_migrate_from_last();
}

/* Move (or insert) a device at the head of the MRU list and persist. */
static void bonded_promote(const struct bt_dev_info* d)
{
    int existing = -1;
    for(int i = 0; i < s_n_bonded; i++) {
        if(memcmp(s_bonded[i].addr, d->addr, 6) == 0) { existing = i; break; }
    }
    if(existing == 0) {
        /* Already at head — refresh name in case the scan picked it up later. */
        if(d->name_set) s_bonded[0] = *d;
        bonded_save();
        return;
    }
    if(existing > 0) {
        struct bt_dev_info hold = s_bonded[existing];
        if(d->name_set) hold = *d;
        for(int i = existing; i > 0; i--) s_bonded[i] = s_bonded[i-1];
        s_bonded[0] = hold;
        bonded_save();
        return;
    }
    int slots = s_n_bonded < BT_SERVICE_MAX_BONDED
                  ? s_n_bonded + 1 : BT_SERVICE_MAX_BONDED;
    for(int i = slots - 1; i > 0; i--) s_bonded[i] = s_bonded[i-1];
    s_bonded[0] = *d;
    s_n_bonded = slots;
    bonded_save();
}

static int bonded_find(const uint8_t addr[6])
{
    for(int i = 0; i < s_n_bonded; i++)
        if(memcmp(s_bonded[i].addr, addr, 6) == 0) return i;
    return -1;
}

static void bonded_remove_at(int idx)
{
    if(idx < 0 || idx >= s_n_bonded) return;
    for(int i = idx; i < s_n_bonded - 1; i++) s_bonded[i] = s_bonded[i+1];
    s_n_bonded--;
    memset(&s_bonded[s_n_bonded], 0, sizeof(s_bonded[s_n_bonded]));
    bonded_save();
}

/* ---- status helpers ---- */

static const char* state_str_for_log(enum bt_state s)
{
    switch(s) {
    case BT_STATE_OFF:        return "OFF";
    case BT_STATE_ENABLING:   return "ENABLING";
    case BT_STATE_READY:      return "READY";
    case BT_STATE_SCANNING:   return "SCANNING";
    case BT_STATE_CONNECTING: return "CONNECTING";
    case BT_STATE_STREAMING:  return "STREAMING";
    case BT_STATE_FAILED:     return "FAILED";
    default:                  return "?";
    }
}

static void set_state(enum bt_state st)
{
    if(st != s_state) {
        crash_log_breadcrumbf("bt: %s -> %s",
                              state_str_for_log(s_state),
                              state_str_for_log(st));
    }
    s_state = st;
}

static void set_status(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status_msg, sizeof(s_status_msg), fmt, ap);
    va_end(ap);
}

static void clear_connected_name(void)
{
    s_connected_name[0] = '\0';
}

static void set_connected_name(const struct bt_dev_info* d)
{
    if(d->name_set) {
        size_t cp = sizeof(s_connected_name) - 1;
        size_t n  = strlen(d->name);
        if(n > cp) n = cp;
        memcpy(s_connected_name, d->name, n);
        s_connected_name[n] = '\0';
    } else {
        snprintf(s_connected_name, sizeof(s_connected_name),
                 "%02X%02X%02X%02X%02X%02X",
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5]);
    }
}

/* ---- device-list management (BT-thread only) ---- */

static int find_dev(const bd_addr_t a)
{
    for(int i = 0; i < s_n_devs; i++)
        if(memcmp(s_devs[i].addr, a, 6) == 0) return i;
    return -1;
}

static void inquiry_add(uint8_t* pkt)
{
    bd_addr_t a;
    gap_event_inquiry_result_get_bd_addr(pkt, a);
    int idx = find_dev(a);
    if(idx < 0) {
        if(s_n_devs >= BT_SERVICE_MAX_DEVS) return;
        idx = s_n_devs++;
        memset(&s_devs[idx], 0, sizeof(s_devs[idx]));
        memcpy(s_devs[idx].addr, a, 6);
    }
    struct bt_dev_info* d = &s_devs[idx];
    if(!d->name_set && gap_event_inquiry_result_get_name_available(pkt)) {
        uint8_t nlen = gap_event_inquiry_result_get_name_len(pkt);
        const uint8_t* n = gap_event_inquiry_result_get_name(pkt);
        size_t copy = nlen < sizeof(d->name) - 1 ? nlen : sizeof(d->name) - 1;
        memcpy(d->name, n, copy);
        d->name[copy] = '\0';
        d->name_set = 1;
    }
    if(gap_event_inquiry_result_get_rssi_available(pkt)) {
        d->rssi = gap_event_inquiry_result_get_rssi(pkt);
        d->rssi_set = 1;
    }
}

/* ---- packet handlers (run in BT thread context) ---- */

/* Forward decl: hci_packet_handler invokes do_connect (defined further
 * down) on the boot-autoconnect path when HCI reaches WORKING. */
static void do_connect(const struct bt_dev_info* d);

static void hci_packet_handler(uint8_t type, uint16_t ch, uint8_t* pkt, uint16_t size)
{
    (void)ch; (void)size;
    if(type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;

    uint8_t evt = hci_event_packet_get_type(pkt);
    switch(evt) {
    case BTSTACK_EVENT_STATE:
        if(btstack_event_state_get_state(pkt) == HCI_STATE_WORKING
           && s_state == BT_STATE_ENABLING) {
            set_state(BT_STATE_READY);
            set_status("ready");
            /* Boot autoconnect arms this so we don't need a UI flow to
             * resume the user's last speaker after power-on. Consume the
             * flag so a later disable+enable doesn't re-trigger. */
            if(s_autoconnect_on_ready && s_n_bonded > 0) {
                s_autoconnect_on_ready = false;
                do_connect(&s_bonded[0]);
            } else {
                s_autoconnect_on_ready = false;
            }
        }
        break;
    case GAP_EVENT_INQUIRY_RESULT:
        if(s_scanning) inquiry_add(pkt);
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        if(s_scanning) {
            s_scanning = false;
            /* Only walk back the state when we were the dedicated scan
             * substate. If we were scanning concurrently with a stream,
             * the stream's state stays untouched — we only clear the
             * scanning flag and refresh the status line. */
            if(s_state == BT_STATE_SCANNING) set_state(BT_STATE_READY);
            set_status("scan: %d found", s_n_devs);
        }
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(pkt, addr);
        gap_ssp_confirmation_response(addr);
        break;

    /* --- Diagnostic instrumentation (see bt-link-log.h).
     *
     * We log link-level events that plausibly correlate with audio
     * cutouts on the Beats Fit Pro path. The current hypothesis space:
     *
     *   - MODE_CHANGE to sniff (0x02): our default policy refuses sniff,
     *     but a slow race between the live ACL coming up and the policy
     *     taking effect would show up here. If we ever see "mode=02"
     *     during a stream we know to nail the policy on the live handle.
     *   - ROLE_CHANGE: we don't request role switch; if the peer forces
     *     one mid-stream it adds ~20-100 ms of dead air.
     *   - FLUSH_OCCURRED: controller dropped a media packet because of a
     *     pending flush timeout. We disabled the watchdog (gotcha 25), so
     *     this should never fire — if it does, something else (peer,
     *     BTstack internals) is requesting flushes.
     *   - MAX_SLOTS_CHANGED / PACKET_TYPE_CHANGED: the negotiated radio
     *     packet length / type set. Useful to confirm BR-only stuck.
     *   - QOS_VIOLATION: the controller says the active QoS contract
     *     was missed for a window of time → likely cluster of retransmits.
     *
     * CONNECTION_COMPLETE doubles as our "fresh session" marker — wipe
     * the log so the user only sees events from the run they care about.
     */
    case HCI_EVENT_CONNECTION_COMPLETE: {
        uint8_t  st  = pkt[2];
        uint16_t hnd = pkt[3] | (pkt[4] << 8);
        bt_link_log_reset();
        bt_link_logf("conn st=%u hnd=%04x", st, hnd);
        break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        uint16_t hnd = pkt[3] | (pkt[4] << 8);
        uint8_t  rsn = pkt[5];
        bt_link_logf("disc hnd=%04x rsn=%02x", hnd, rsn);
        /* AVDTP layer will follow up with STREAM_RELEASED; reset state there. */
        if(rsn != 0) set_status("dis rsn=%02x", rsn);
        break;
    }
    case HCI_EVENT_FLUSH_OCCURRED: {
        uint16_t hnd = pkt[2] | (pkt[3] << 8);
        bt_link_logf("FLUSH hnd=%04x", hnd);
        break;
    }
    case HCI_EVENT_ROLE_CHANGE: {
        uint8_t  st   = pkt[2];
        uint8_t  role = pkt[9];
        bt_link_logf("role st=%u r=%u", st, role);
        break;
    }
    case HCI_EVENT_MODE_CHANGE: {
        uint8_t  st       = pkt[2];
        uint16_t hnd      = pkt[3] | (pkt[4] << 8);
        uint8_t  mode     = pkt[5];
        uint16_t interval = pkt[6] | (pkt[7] << 8);
        bt_link_logf("mode st=%u h=%04x m=%u iv=%u", st, hnd, mode, interval);
        break;
    }
    case HCI_EVENT_MAX_SLOTS_CHANGED: {
        uint16_t hnd  = pkt[2] | (pkt[3] << 8);
        uint8_t  lmps = pkt[4];
        bt_link_logf("slots h=%04x max=%u", hnd, lmps);
        break;
    }
    case HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED: {
        uint8_t  st   = pkt[2];
        uint16_t hnd  = pkt[3] | (pkt[4] << 8);
        uint16_t types = pkt[5] | (pkt[6] << 8);
        bt_link_logf("ptype st=%u h=%04x t=%04x", st, hnd, types);
        break;
    }
    case HCI_EVENT_QOS_VIOLATION: {
        uint16_t hnd = pkt[2] | (pkt[3] << 8);
        bt_link_logf("QOSV h=%04x", hnd);
        break;
    }
    case HCI_EVENT_LINK_SUPERVISION_TIMEOUT_CHANGED: {
        uint16_t hnd = pkt[2] | (pkt[3] << 8);
        uint16_t to  = pkt[4] | (pkt[5] << 8);
        bt_link_logf("lsup h=%04x to=%u", hnd, to);
        break;
    }
    default:
        break;
    }
}

static void a2dp_packet_handler(uint8_t type, uint16_t ch, uint8_t* pkt, uint16_t size)
{
    (void)ch; (void)size;
    if(type != HCI_EVENT_PACKET) return;
    if(hci_event_packet_get_type(pkt) != HCI_EVENT_A2DP_META) return;

    uint8_t sub = hci_event_a2dp_meta_get_subevent_code(pkt);
    switch(sub) {
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
        uint8_t st = a2dp_subevent_signaling_connection_established_get_status(pkt);
        if(st != 0) {
            set_state(BT_STATE_FAILED);
            set_status("sig fail %u", st);
            break;
        }
        /* Pick up the cid (works for both outgoing connects and incoming
         * connections from the speaker auto-reconnecting on power-on)
         * and transition to CONNECTING so a subsequent connect_last call
         * does not redundantly try to set up a stream that BTstack will
         * already drive to STREAM_ESTABLISHED on its own. */
        s_a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(pkt);
        if(s_state == BT_STATE_READY) set_state(BT_STATE_CONNECTING);
        set_status("signaling ok");
        bd_addr_t addr;
        a2dp_subevent_signaling_connection_established_get_bd_addr(pkt, addr);
        if(!s_picked_valid) {
            memset(&s_picked, 0, sizeof(s_picked));
            memcpy(s_picked.addr, addr, 6);
            /* If this address matches a known bonded device, inherit its
             * name so the UI shows "On: <speaker name>" instead of the bare
             * MAC for an unsolicited reconnect. */
            int b = bonded_find(addr);
            if(b >= 0) s_picked = s_bonded[b];
            s_picked_valid = true;
        }
        /* Initiate AVRCP from the source side. Many sinks (Beats Fit Pro
         * confirmed) won't open an AVCTP channel on their own — they wait
         * for the controller-capable peer to connect, then send PASSTHROUGH
         * commands over the established channel. */
        if(s_avrcp_cid == 0) {
            uint16_t cid = 0;
            uint8_t rc = avrcp_connect(addr, &cid);
            set_status("avrcp_connect rc=%u", rc);
        }
        break;
    }
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        uint16_t freq =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(pkt);
        uint8_t bl =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(pkt);
        uint8_t sb =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(pkt);
        uint8_t mp =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(pkt);
        uint8_t al =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(pkt);
        uint8_t cm =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(pkt);
        bt_pcm_sink_set_sbc_config(freq, bl, sb, al, cm, mp);
        break;
    }
    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        uint8_t st = a2dp_subevent_stream_established_get_status(pkt);
        if(st == 0) {
            if(s_picked_valid && !s_saved_this_session) {
                bonded_promote(&s_picked);
                s_saved_this_session = true;
            }
            (void)a2dp_source_start_stream(s_a2dp_cid, s_local_seid);
        } else {
            set_state(BT_STATE_FAILED);
            set_status("stream fail %u", st);
        }
        break;
    }
    case A2DP_SUBEVENT_STREAM_STARTED:
        bt_pcm_sink_start_streaming(s_a2dp_cid, s_local_seid);
        pcm_set_current_sink(PCM_SINK_BT);
        if(s_picked_valid) set_connected_name(&s_picked);
        set_state(BT_STATE_STREAMING);
        set_status("streaming");
        break;
    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        bt_pcm_sink_handle_can_send_now();
        break;
    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        bt_pcm_sink_stop_streaming();
        pcm_set_current_sink(PCM_SINK_BUILTIN);
        set_state(BT_STATE_READY);
        set_status("suspended");
        break;
    case A2DP_SUBEVENT_STREAM_RELEASED:
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        bt_pcm_sink_stop_streaming();
        pcm_set_current_sink(PCM_SINK_BUILTIN);
        clear_connected_name();
        set_state(BT_STATE_READY);
        set_status("disconnected");
        s_picked_valid = false;
        s_a2dp_cid = 0;
        /* Switch-device handoff: a CONNECT_ADDR posted while we were
         * streaming asked us to disconnect-then-connect. The disconnect
         * just landed; pick up the saved target now. */
        if(s_pending_switch) {
            s_pending_switch = false;
            struct bt_dev_info d;
            memset(&d, 0, sizeof(d));
            memcpy(d.addr, s_pending_switch_addr, 6);
            int idx = find_dev(s_pending_switch_addr);
            if(idx >= 0) d = s_devs[idx];
            else {
                int b = bonded_find(s_pending_switch_addr);
                if(b >= 0) d = s_bonded[b];
            }
            do_connect(&d);
        }
        break;
    default:
        break;
    }
}

/* ---- AVRCP target: receive PASSTHROUGH commands from the sink (e.g.
 *      Beats Fit Pro single-tap → PLAY/PAUSE) and route them to the
 *      Rockbox playback engine. PLAY / PAUSE / FORWARD / BACKWARD
 *      / STOP only; metadata, volume sync, and notifications back to
 *      the sink are out of v1 scope.
 *
 *      All audio_*() entry points post to the audio queue and are
 *      cross-thread safe, so calling them from the BT thread is fine. */

static void avrcp_packet_handler(uint8_t type, uint16_t ch, uint8_t* pkt, uint16_t size)
{
    (void)ch; (void)size;
    if(type != HCI_EVENT_PACKET) return;
    if(hci_event_packet_get_type(pkt) != HCI_EVENT_AVRCP_META) return;

    switch(pkt[2]) {
    case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
        uint8_t st = avrcp_subevent_connection_established_get_status(pkt);
        if(st != ERROR_CODE_SUCCESS) {
            set_status("avrcp fail %u", st);
            break;
        }
        s_avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(pkt);
        set_status("avrcp on cid=%04x", s_avrcp_cid);
        break;
    }
    case AVRCP_SUBEVENT_CONNECTION_RELEASED:
        s_avrcp_cid = 0;
        set_status("avrcp off");
        break;
    default:
        set_status("avrcp evt %02x", pkt[2]);
        break;
    }
}

static void avrcp_target_packet_handler(uint8_t type, uint16_t ch, uint8_t* pkt, uint16_t size)
{
    (void)ch; (void)size;
    if(type != HCI_EVENT_PACKET) return;
    if(hci_event_packet_get_type(pkt) != HCI_EVENT_AVRCP_META) return;

    /* Show every target subevent so we can tell whether PASSTHROUGH is
     * arriving but unhandled, vs. nothing arriving at all. */
    if(pkt[2] != AVRCP_SUBEVENT_OPERATION) {
        set_status("avrcp tgt evt %02x", pkt[2]);
        return;
    }

    bool pressed = avrcp_subevent_operation_get_button_pressed(pkt) > 0;
    avrcp_operation_id_t op =
        (avrcp_operation_id_t)avrcp_subevent_operation_get_operation_id(pkt);

    set_status("avrcp op=%02x %s", (unsigned)op, pressed ? "pr" : "rel");

    /* AVRCP PASSTHROUGH frames arrive twice (PRESS then RELEASE).
     * Act on PRESS only so a single tap doesn't invoke twice. */
    if(!pressed) return;

    /* Map AVRCP operation → Rockbox playback API.
     * STOP is mapped to pause (rather than audio_stop) so the user can
     * resume from the bud without having to navigate Rockbox after a
     * stray triple-tap.
     *
     * PLAY and PAUSE are both treated as a toggle against current playback
     * state. Apple H1 buds (Beats Fit Pro) only ever send PAUSE because
     * without our playback-status notifications they assume the source is
     * still in the "playing" state, so a non-toggling map would only work
     * for the very first tap. */
    int st = audio_status();
    bool playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    switch(op) {
    case AVRCP_OPERATION_ID_PLAY:
    case AVRCP_OPERATION_ID_PAUSE:
        if(playing) audio_pause(); else audio_resume();
        break;
    case AVRCP_OPERATION_ID_STOP:     audio_pause();  break;
    case AVRCP_OPERATION_ID_FORWARD:  audio_next();   break;
    case AVRCP_OPERATION_ID_BACKWARD: audio_prev();   break;
    default: break;
    }
}

/* ---- BT thread command processing ---- */

static void do_connect(const struct bt_dev_info* d)
{
    s_picked = *d;
    s_picked_valid = true;
    s_saved_this_session = false;
    set_state(BT_STATE_CONNECTING);
    set_status("connecting");
    bd_addr_t addr;
    memcpy(addr, d->addr, 6);
    uint8_t rc = a2dp_source_establish_stream(addr, &s_a2dp_cid);
    /* ERROR_CODE_COMMAND_DISALLOWED (0x0C, decimal 12) means there is
     * already a connection or stream-establish flow underway for this
     * address — typically because the speaker auto-reconnected as soon
     * as our chip came up and beat us to the punch. Per a2dp_source.c's
     * comment, "the stream will get set-up nevertheless" — so leave the
     * state as CONNECTING and let the SIGNALING_CONNECTION_ESTABLISHED
     * + STREAM_ESTABLISHED events drive us to STREAMING. */
    if(rc != 0 && rc != ERROR_CODE_COMMAND_DISALLOWED) {
        set_state(BT_STATE_FAILED);
        set_status("a2dp rc=%u", rc);
    }
}

static void do_disconnect(void)
{
    /* Best-effort tear down: stop the stream if running, then disconnect
     * the ACL link. The packet handlers transition state on the resulting
     * RELEASED / DISCONNECTION_COMPLETE events. */
    if(s_a2dp_cid && (s_state == BT_STATE_STREAMING
                      || s_state == BT_STATE_CONNECTING)) {
        a2dp_source_disconnect(s_a2dp_cid);
    }
}

static void process_pending_command(void)
{
    if(!s_cmd_pending) return;
    int cmd = s_cmd;
    s_cmd_pending = false;

    switch(cmd) {
    case BT_CMD_DISABLE:
        s_thread_running = false;     /* main loop will exit */
        break;
    case BT_CMD_SCAN_START:
        if(s_scanning) break;       /* already scanning */
        if(s_state == BT_STATE_OFF || s_state == BT_STATE_ENABLING) break;
        s_n_devs = 0;
        s_scanning = true;
        /* Only flip the visible state to SCANNING when nothing else is
         * occupying it; otherwise (STREAMING/CONNECTING) leave it alone
         * so the UI keeps showing the active connection. */
        if(s_state == BT_STATE_READY || s_state == BT_STATE_FAILED)
            set_state(BT_STATE_SCANNING);
        set_status("scanning");
        gap_inquiry_start(8);          /* 8 * 1.28s ≈ 10s */
        break;
    case BT_CMD_SCAN_STOP:
        if(s_scanning) gap_inquiry_stop();   /* triggers INQUIRY_COMPLETE */
        break;
    case BT_CMD_CONNECT_LAST:
        if(s_state == BT_STATE_READY && s_n_bonded > 0)
            do_connect(&s_bonded[0]);
        break;
    case BT_CMD_CONNECT_ADDR: {
        if(s_state == BT_STATE_OFF || s_state == BT_STATE_ENABLING) break;
        /* If a stream is up to a different device, the user wants to
         * switch — disconnect first and stash the new addr so the
         * STREAM_RELEASED handler can issue the new connect once the
         * radio is free. Same address: noop. */
        if(s_state == BT_STATE_STREAMING || s_state == BT_STATE_CONNECTING) {
            if(s_picked_valid && memcmp(s_picked.addr, s_cmd_addr, 6) == 0)
                break;
            memcpy(s_pending_switch_addr, s_cmd_addr, 6);
            s_pending_switch = true;
            do_disconnect();
            break;
        }
        struct bt_dev_info d;
        memset(&d, 0, sizeof(d));
        memcpy(d.addr, s_cmd_addr, 6);
        /* Inherit name from scan results, then fall back to bonded list, so
         * a connect-by-address still picks up a friendly name when the
         * caller (UI) only had the address handy. */
        int idx = find_dev(s_cmd_addr);
        if(idx >= 0) d = s_devs[idx];
        else {
            int b = bonded_find(s_cmd_addr);
            if(b >= 0) d = s_bonded[b];
        }
        do_connect(&d);
        break;
    }
    case BT_CMD_DISCONNECT:
        do_disconnect();
        break;
    case BT_CMD_FORGET: {
        /* If we're streaming/connecting to this address, tear down first
         * so the speaker doesn't think it's still bonded after we drop the
         * link key locally. The packet handlers will reset state on the
         * resulting RELEASED event. */
        if(s_picked_valid && memcmp(s_picked.addr, s_cmd_addr, 6) == 0
           && (s_state == BT_STATE_STREAMING
               || s_state == BT_STATE_CONNECTING)) {
            do_disconnect();
        }
        bd_addr_t a;
        memcpy(a, s_cmd_addr, 6);
        gap_drop_link_key_for_bd_addr(a);
        int idx = bonded_find(s_cmd_addr);
        if(idx >= 0) bonded_remove_at(idx);
        set_status("forgot device");
        break;
    }
    default:
        break;
    }
}

/* ---- BT thread main ---- */

static void bt_thread_main(void)
{
    set_state(BT_STATE_ENABLING);
    set_status("power: on");

    bt_hw_power(true);

    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    uint8_t ev_buf[16];
    if(bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                         ev_buf, sizeof(ev_buf), 2000) < 7) {
        set_status("rst1 fail"); goto thread_done;
    }
    set_status("patchram");
    if(bt_bcm_patchram_upload(BT_BCM_FW_PATH) <= 0) {
        set_status("patchram fail"); goto thread_done;
    }
    mdelay(500);
    if(bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                         ev_buf, sizeof(ev_buf), 2000) < 7) {
        set_status("rst2 fail"); goto thread_done;
    }

    /* Switch UART to 3 Mbps via BCM vendor cmd 0xFC18. See gotcha #14. */
    {
        const uint32_t new_baud = 3000000;
        uint8_t baud_cmd[10] = {
            0x01, 0x18, 0xFC, 0x06,
            0x00, 0x00,
            (uint8_t)(new_baud      ),
            (uint8_t)(new_baud >>  8),
            (uint8_t)(new_baud >> 16),
            (uint8_t)(new_baud >> 24),
        };
        if(bt_hci_cmd_reply(baud_cmd, sizeof(baud_cmd),
                             ev_buf, sizeof(ev_buf), 1000) < 7
           || ev_buf[6] != 0x00) {
            set_status("baud cmd fail"); goto thread_done;
        }
        mdelay(50);
        hal_uart_dma_set_baud(new_baud);
        mdelay(50);
        bt_hw_drain_rx();
        static const uint8_t hci_read_local_ver[] = { 0x01, 0x01, 0x10, 0x00 };
        if(bt_hci_cmd_reply(hci_read_local_ver, sizeof(hci_read_local_ver),
                             ev_buf, sizeof(ev_buf), 1000) < 7) {
            set_status("3M probe fail"); goto thread_done;
        }
    }

    hal_uart_dma_init();
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    static hci_transport_config_uart_t cfg = {
        .type          = HCI_TRANSPORT_CONFIG_UART,
        .baudrate_init = 3000000,
        .baudrate_main = 0,
        .flowcontrol   = 1,
        .device_name   = NULL,
        .parity        = 0,
    };
    hci_init(hci_transport_h4_instance(btstack_uart_block_embedded_instance()),
             &cfg);

    bt_tlv_init();
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(&bt_tlv_impl, NULL));
    hci_set_master_slave_policy(0);
    /* Forbid sniff/hold/park on all ACLs. Apple H1-class sinks (AirPods,
     * Beats) try to enter sniff for battery; the active↔sniff transitions
     * stall media for ~100 ms and show up as periodic hitching during
     * playback. With link policy = 0 the local LM refuses sniff requests
     * from the peer, keeping the link in active mode for the duration. */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_DISABLE_ALL_LM_MODES);
    /* Restrict ACL packet types to basic-rate (1 Mbps GFSK) only — disable
     * EDR 2-DH* / 3-DH* on all classic links. EDR needs ~5-9 dB more SNR
     * than BR to stay below threshold; in a body-blocking null (e.g. F20
     * in breast pocket, head turned to put the skull between source and
     * primary bud) that's exactly the margin lost, and EDR drops into a
     * retransmit cascade that the user hears as a multi-hundred-ms tear
     * or a sustained dropout. BR rides through the same null with frame
     * loss instead of cascade collapse.
     *
     * Bandwidth check: SBC bitpool 35, 44.1 kHz, joint stereo is ~250 kbps
     * payload. 1-DH5 carries ~700 kbps usable, so BR has ample headroom
     * and we lose no audio quality. Bose / Redmi / similar sinks are
     * unaffected — they were never bandwidth-limited at our bitpool. */
    hci_enable_acl_packet_types(ACL_PACKET_TYPES_BR);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    l2cap_init();
    sdp_init();

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_packet_handler);

    avdtp_stream_endpoint_t* ep = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        sbc_caps, sizeof(sbc_caps),
        sbc_config, sizeof(sbc_config));
    if(!ep) { set_status("ep alloc fail"); goto thread_done; }
    s_local_seid = avdtp_local_seid(ep);

    static uint8_t sdp_buf[150];
    a2dp_source_create_sdp_record(sdp_buf, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER,
                                  NULL, NULL);
    sdp_register_service(sdp_buf);

    /* AVRCP target — receives PLAY/PAUSE/NEXT/PREV from the sink button.
     * BTstack ties target and controller together at the protocol layer,
     * so both halves get init'd even though we only consume target events.
     * Two SDP records: target advertises CATEGORY_PLAYER_OR_RECORDER (the
     * sink is allowed to send Category 1 commands like play/pause to us);
     * controller advertises CATEGORY_MONITOR_OR_AMPLIFIER (we don't act
     * on it in v1 but the sink expects to see it). */
    avrcp_init();
    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_packet_handler);

    static uint8_t sdp_avrcp_target_buf[200];
    avrcp_target_create_sdp_record(sdp_avrcp_target_buf,
        sdp_create_service_record_handle(),
        AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER,
        NULL, NULL);
    sdp_register_service(sdp_avrcp_target_buf);

    static uint8_t sdp_avrcp_controller_buf[200];
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_buf,
        sdp_create_service_record_handle(),
        AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER,
        NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_buf);

    gap_set_local_name("Rockbox F20");
    gap_set_class_of_device(0x240404);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_set_bondable_mode(1);
    gap_secure_connections_enable(false);
    gap_set_security_level(LEVEL_2);

    static btstack_packet_callback_registration_t reg;
    reg.callback = &hci_packet_handler;
    hci_add_event_handler(&reg);

    /* Pre-load remembered devices BEFORE hci_power_control so the
     * BTSTACK_EVENT_STATE → WORKING handler can use s_bonded for the boot
     * autoconnect path without racing the file load. */
    bonded_load();

    set_status("hci power on");
    hci_power_control(HCI_POWER_ON);

    /* Service main loop. Exits only when a DISABLE command is posted. */
    while(s_thread_running) {
        btstack_run_loop_embedded_execute_once();
        process_pending_command();
    }

    /* Shutdown: stop streaming + disconnect, then power off the chip. */
    if(bt_pcm_sink_is_active()) {
        bt_pcm_sink_stop_streaming();
        pcm_set_current_sink(PCM_SINK_BUILTIN);
    }
    do_disconnect();
    long post = current_tick + HZ;
    while(!TIME_AFTER(current_tick, post))
        btstack_run_loop_embedded_execute_once();
    hci_power_control(HCI_POWER_OFF);

thread_done:
    bt_hw_power(false);
    set_state(BT_STATE_OFF);
    set_status("off");
    s_thread_running = false;
    bt_btstack_hal_signal();   /* in case anything is mid-wait */
}

/* ---- public API ---- */

int bt_service_enable(void)
{
    if(s_state != BT_STATE_OFF) return 0;     /* idempotent */
    bt_btstack_hal_init();
    s_n_devs              = 0;
    s_a2dp_cid            = 0;
    s_picked_valid        = false;
    s_saved_this_session  = false;
    s_autoconnect_on_ready = false;
    s_scanning            = false;
    s_pending_switch      = false;
    s_cmd                 = BT_CMD_NONE;
    s_cmd_pending         = false;
    s_status_msg[0]       = '\0';
    s_connected_name[0]   = '\0';
    s_thread_running      = true;
    set_state(BT_STATE_ENABLING);
    s_thread_id = create_thread(bt_thread_main,
                                 s_stack, sizeof(s_stack),
                                 0, s_thread_name
                                 IF_PRIO(, PRIORITY_PLAYBACK_MAX)
                                 IF_COP(, CPU));
    return 0;
}

int bt_service_enable_and_connect_last(void)
{
    /* Set the flag BEFORE spawning the thread so it's visible by the
     * time the BTSTACK_EVENT_STATE → WORKING handler runs. If the service
     * is already enabled, just connect the bonded[0] now (synchronous
     * post). */
    if(s_state == BT_STATE_OFF) {
        bt_service_enable();
        s_autoconnect_on_ready = true;
        return 0;
    }
    if(s_state == BT_STATE_READY)
        return bt_service_connect_last();
    return 0;
}

int bt_service_disable(void)
{
    if(s_state == BT_STATE_OFF) return 0;
    s_cmd = BT_CMD_DISABLE;
    s_cmd_pending = true;
    bt_btstack_hal_signal();
    thread_wait(s_thread_id);
    return 0;
}

bool bt_service_is_enabled(void)
{
    return s_state != BT_STATE_OFF;
}

static int post_cmd(int cmd)
{
    if(!bt_service_is_enabled()) return -1;
    s_cmd = cmd;
    s_cmd_pending = true;
    bt_btstack_hal_signal();
    return 0;
}

int bt_service_scan_start(void)    { return post_cmd(BT_CMD_SCAN_START); }
int bt_service_scan_stop(void)     { return post_cmd(BT_CMD_SCAN_STOP); }
int bt_service_connect_last(void)  { return post_cmd(BT_CMD_CONNECT_LAST); }
int bt_service_disconnect(void)    { return post_cmd(BT_CMD_DISCONNECT); }

int bt_service_connect_addr(const uint8_t addr[6])
{
    if(!bt_service_is_enabled()) return -1;
    memcpy(s_cmd_addr, addr, 6);
    s_cmd = BT_CMD_CONNECT_ADDR;
    s_cmd_pending = true;
    bt_btstack_hal_signal();
    return 0;
}

int bt_service_forget(const uint8_t addr[6])
{
    if(!bt_service_is_enabled()) return -1;
    memcpy(s_cmd_addr, addr, 6);
    s_cmd = BT_CMD_FORGET;
    s_cmd_pending = true;
    bt_btstack_hal_signal();
    return 0;
}

enum bt_state bt_service_get_state(void)        { return s_state; }
bool          bt_service_is_scanning(void)      { return s_scanning; }
const char*   bt_service_get_status_msg(void)   { return s_status_msg; }
const char*   bt_service_get_connected_name(void) { return s_connected_name; }
bool          bt_service_have_last(void)         { return s_n_bonded > 0; }
const struct bt_dev_info* bt_service_get_last(void)
{
    return s_n_bonded > 0 ? &s_bonded[0] : NULL;
}

int bt_service_get_scan_results(struct bt_dev_info* out, int max)
{
    int n = s_n_devs;
    if(n > max) n = max;
    /* Snapshot under IRQ disable to avoid a tear with the BT thread
     * mutating s_devs during inquiry_add. The cost is small (a few μs). */
    int irq = disable_irq_save();
    memcpy(out, s_devs, n * sizeof(struct bt_dev_info));
    restore_irq(irq);
    return n;
}

int bt_service_get_bonded(struct bt_dev_info* out, int max)
{
    int n = s_n_bonded;
    if(n > max) n = max;
    int irq = disable_irq_save();
    memcpy(out, s_bonded, n * sizeof(struct bt_dev_info));
    restore_irq(irq);
    return n;
}

#endif /* !BOOTLOADER */
