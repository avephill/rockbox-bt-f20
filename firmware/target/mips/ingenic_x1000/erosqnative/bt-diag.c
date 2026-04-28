/***************************************************************************
 * BT diagnostic screen for EROS Q Native / Surfans F20 (HW4).
 *
 * Exercises the bt-erosqnative transport and the BCM patchram upload, then
 * queries chip identity via HCI. All heavy lifting is in the transport and
 * patchram modules — this file is display + sequencing only.
 *
 * Copyright (C) 2024-2026 - GPLv2
 ****************************************************************************/

#ifndef BOOTLOADER
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "action.h"
#include "kernel.h"
#include "button.h"

#include "bt-erosqnative.h"
#include "bt-bcm-patchram.h"

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

extern const btstack_uart_t * btstack_uart_block_embedded_instance(void);

#include "btstack_link_key_db_memory.h"
#include "classic/sdp_server.h"
#include "l2cap.h"

static void putline(int* row, const char* s)
{
    lcd_puts(0, (*row)++, s);
    lcd_update();
}

static void dump_hex(int* row, const uint8_t* b, int n)
{
    char hex[48]; int hlen = 0;
    for(int i = 0; i < n && hlen < (int)sizeof(hex) - 3; i++)
        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", b[i]);
    putline(row, hex);
}

bool dbg_bt_diag(void)
{
    int row = 0;
    char ln[52];
    uint8_t ev[32];

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    putline(&row, "BT diag");

    /* 1. Power up and mux UART */
    bt_hw_power(true);
    putline(&row, "power: on");

    /* 2. HCI Reset (chip is in ROM mode) */
    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    int n = bt_hci_cmd_reply(hci_reset, sizeof(hci_reset), ev, sizeof(ev), 2000);
    snprintf(ln, sizeof(ln), "rst1: n=%d", n);
    putline(&row, ln);
    if(n < 7) goto done;
    dump_hex(&row, ev, n);

    /* 3. Upload patchram firmware */
    int up = bt_bcm_patchram_upload(BT_BCM_FW_PATH);
    snprintf(ln, sizeof(ln), "patchram: %d", up);
    putline(&row, ln);
    if(up <= 0) goto done;

    /* 4. Chip reboots into full firmware */
    mdelay(500);

    /* 5. HCI Reset again — proves firmware is running */
    n = bt_hci_cmd_reply(hci_reset, sizeof(hci_reset), ev, sizeof(ev), 2000);
    snprintf(ln, sizeof(ln), "rst2: n=%d", n);
    putline(&row, ln);
    if(n < 7) goto done;

    /* 6. Read BD_ADDR (0x1009) */
    static const uint8_t c_bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
    n = bt_hci_cmd_reply(c_bdaddr, sizeof(c_bdaddr), ev, sizeof(ev), 500);
    if(n >= 13 && ev[6] == 0) {
        snprintf(ln, sizeof(ln), "BD %02X:%02X:%02X:%02X:%02X:%02X",
                 ev[12], ev[11], ev[10], ev[9], ev[8], ev[7]);
    } else {
        snprintf(ln, sizeof(ln), "BD fail n=%d", n);
    }
    putline(&row, ln);

    /* 7. Read Local Version (0x1001) */
    static const uint8_t c_ver[] = { 0x01, 0x01, 0x10, 0x00 };
    n = bt_hci_cmd_reply(c_ver, sizeof(c_ver), ev, sizeof(ev), 500);
    if(n >= 15 && ev[6] == 0) {
        uint16_t mfr = ev[11] | (ev[12] << 8);
        uint16_t lmp_sub = ev[13] | (ev[14] << 8);
        snprintf(ln, sizeof(ln), "HCI=%d LMP=%d mfr=%d",
                 ev[7], ev[10], mfr);
        putline(&row, ln);
        snprintf(ln, sizeof(ln), "lmp_sub=0x%04x", lmp_sub);
        putline(&row, ln);
    } else {
        snprintf(ln, sizeof(ln), "ver fail n=%d", n);
        putline(&row, ln);
    }

    /* 8. BCM vendor Update_Baudrate (0xFC18) -> switch to 3 Mbps
     * Params: 2 bytes reserved (0x00 0x00) + 4 bytes baud little-endian */
    uint8_t baud_cmd[10] = {
        0x01, 0x18, 0xFC, 0x06,
        0x00, 0x00,
        (uint8_t)(BT_UART_BAUD_HS      ),
        (uint8_t)(BT_UART_BAUD_HS >>  8),
        (uint8_t)(BT_UART_BAUD_HS >> 16),
        (uint8_t)(BT_UART_BAUD_HS >> 24),
    };
    int st = bt_hci_cmd(baud_cmd, sizeof(baud_cmd), 500);
    snprintf(ln, sizeof(ln), "baud: st=%d", st);
    putline(&row, ln);
    if(st == 0) {
        /* Chip acknowledged; now switch our UART and verify */
        mdelay(20);
        bt_hw_set_baud(BT_UART_BAUD_HS);

        n = bt_hci_cmd_reply(c_bdaddr, sizeof(c_bdaddr), ev, sizeof(ev), 500);
        if(n >= 13 && ev[6] == 0) {
            snprintf(ln, sizeof(ln), "3M BD %02X:%02X:%02X:%02X:%02X:%02X",
                     ev[12], ev[11], ev[10], ev[9], ev[8], ev[7]);
        } else {
            snprintf(ln, sizeof(ln), "3M fail n=%d", n);
        }
        putline(&row, ln);
    }

done:
    bt_hw_power(false);
    putline(&row, "---");
    lcd_puts(0, 15, "BACK to exit");
    lcd_update();
    while(get_action(CONTEXT_STD, HZ) != ACTION_STD_CANCEL);
    return false;
}

/* ============================================================================
 * BTstack inquiry demo — next step: get BTstack to do HCI inquiry on top of
 * our transport. This exercises: hal_uart_dma, btstack_uart_block_embedded,
 * hci_transport_h4, hci core, gap inquiry.
 * ============================================================================ */

static int  s_row;
static int  s_found;
static bool s_done;
static char s_latch[32];   /* persistent bottom line (e.g. "BOND st=0") */

static void redraw_latch(void)
{
    if(s_latch[0])
        lcd_puts(0, 14, s_latch);
}

static void print_evt(const char* s)
{
    if(s_row >= 14) {
        /* Screen full — clear and restart from row 0. Re-draw the latched
         * status line (if any) so the final result survives the scroll. */
        lcd_clear_display();
        s_row = 0;
        redraw_latch();
    }
    lcd_puts(0, s_row++, s);
    lcd_update();
}

static void set_latch(const char* s)
{
    size_t i;
    for(i = 0; i < sizeof(s_latch) - 1 && s[i]; i++)
        s_latch[i] = s[i];
    s_latch[i] = '\0';
    redraw_latch();
    lcd_update();
}

static void inquiry_packet_handler(uint8_t packet_type, uint16_t channel,
                                   uint8_t* packet, uint16_t size)
{
    (void)channel; (void)size;
    char ln[52];
    bd_addr_t addr;
    if(packet_type != HCI_EVENT_PACKET) return;

    uint8_t ev = hci_event_packet_get_type(packet);
    switch(ev) {
        case BTSTACK_EVENT_STATE:
            if(btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                print_evt("HCI up, inquiry...");
                gap_inquiry_start(8);   /* 8 * 1.28s ~= 10.24s */
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            snprintf(ln, sizeof(ln), "%d: %02X%02X%02X%02X%02X%02X",
                     s_found++, addr[0], addr[1], addr[2],
                                addr[3], addr[4], addr[5]);
            print_evt(ln);
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            snprintf(ln, sizeof(ln), "inquiry done (%d)", s_found);
            print_evt(ln);
            s_done = true;
            break;
    }
}

bool dbg_bt_inquiry(void)
{
    s_row = 0;
    s_found = 0;
    s_done = false;

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    print_evt("BT inquiry");

    /* Bring up chip with our proven polled path, then upload firmware */
    bt_hw_power(true);
    print_evt("power: on");

    /* HCI Reset over polled transport, then patchram */
    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    uint8_t ev_[16];
    int n = bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                              ev_, sizeof(ev_), 2000);
    if(n < 7) { print_evt("rst1 fail"); goto done; }

    int up = bt_bcm_patchram_upload(BT_BCM_FW_PATH);
    if(up <= 0) { print_evt("patchram fail"); goto done; }
    mdelay(500);

    /* Post-patchram HCI Reset (chip now in full firmware) */
    n = bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                         ev_, sizeof(ev_), 2000);
    if(n < 7) { print_evt("rst2 fail"); goto done; }
    print_evt("fw ready");

    /* Hand the UART over to BTstack's interrupt-driven transport */
    hal_uart_dma_init();

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    static hci_transport_config_uart_t cfg = {
        .type          = HCI_TRANSPORT_CONFIG_UART,
        .baudrate_init = BT_UART_BAUD_INIT,
        .baudrate_main = 0,
        .flowcontrol   = 1,
        .device_name   = NULL,
        .parity        = 0,
    };
    hci_init(hci_transport_h4_instance(btstack_uart_block_embedded_instance()),
             &cfg);

    static btstack_packet_callback_registration_t reg;
    reg.callback = &inquiry_packet_handler;
    hci_add_event_handler(&reg);

    print_evt("hci_power on");
    hci_power_control(HCI_POWER_ON);

    /* Poll run loop for up to ~15s or until GAP_EVENT_INQUIRY_COMPLETE */
    long deadline = current_tick + 15 * HZ;
    while(!s_done && !TIME_AFTER(current_tick, deadline)) {
        btstack_run_loop_embedded_execute_once();
        if(get_action(CONTEXT_STD, 0) == ACTION_STD_CANCEL)
            break;
    }

    hci_power_control(HCI_POWER_OFF);

done:
    bt_hw_power(false);
    print_evt("---");
    lcd_puts(0, 15, "BACK to exit");
    lcd_update();
    while(get_action(CONTEXT_STD, HZ) != ACTION_STD_CANCEL);
    return false;
}

/* ============================================================================
 * BT pair demo — enter discoverable / pairable mode and print SSP events.
 * Use this with a phone: scan for "Rockbox F20" and tap to pair.
 * ============================================================================ */

static bool s_peer_set;

static void pair_packet_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t* packet, uint16_t size)
{
    (void)channel; (void)size;
    char ln[52];
    bd_addr_t addr;
    link_key_t link_key;
    link_key_type_t lk_type;
    if(packet_type != HCI_EVENT_PACKET) return;

    uint8_t ev = hci_event_packet_get_type(packet);
    switch(ev) {
        case BTSTACK_EVENT_STATE:
            if(btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                /* Hardcoded Bose address from earlier BT Inquiry —
                 * E4:58:BC:FD:B3:68. Skip inquiry to keep events clean. */
                bd_addr_t bose = { 0xE4, 0x58, 0xBC, 0xFD, 0xB3, 0x68 };
                int rc = gap_dedicated_bonding(bose, 0);
                snprintf(ln, sizeof(ln), "bond-> rc=%d", rc);
                print_evt(ln);
                if(rc != 0) s_done = true;
            }
            break;
        case GAP_EVENT_DEDICATED_BONDING_COMPLETED:
            /* Latch result on row 14 so it survives scroll-clears.
             * Row 15 is for "BACK to exit" printed after cleanup. */
            snprintf(ln, sizeof(ln), "BOND st=%d", packet[2]);
            set_latch(ln);
            print_evt(ln);
            s_done = true;
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            /* Legacy pairing (rare on modern phones). Accept '0000'. */
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            gap_pin_code_response(addr, "0000");
            print_evt("pin 0000");
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            /* SSP Just Works / numeric comparison. Auto-confirm explicitly. */
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            uint32_t num = hci_event_user_confirmation_request_get_numeric_value(packet);
            gap_ssp_confirmation_response(addr);
            snprintf(ln, sizeof(ln), "confirm %06lu", (unsigned long)num);
            print_evt(ln);
            break;
        }
        case HCI_EVENT_USER_PASSKEY_REQUEST:
            print_evt("passkey req");
            break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            print_evt("passkey notif");
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
            snprintf(ln, sizeof(ln), "SSP %02X%02X%02X st=%d",
                     addr[3], addr[4], addr[5],
                     hci_event_simple_pairing_complete_get_status(packet));
            print_evt(ln);
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            /* Packet layout: [0]=evcode [1]=plen [2..7]=bd_addr
             *                [8..23]=link_key [24]=link_key_type */
            reverse_bd_addr(&packet[2], addr);
            memcpy(link_key, &packet[8], 16);
            lk_type = (link_key_type_t)packet[24];
            snprintf(ln, sizeof(ln), "LK %02X%02X%02X%02X.. t=%d",
                     link_key[0], link_key[1], link_key[2], link_key[3],
                     lk_type);
            print_evt(ln);
            break;
        case HCI_EVENT_CONNECTION_REQUEST:
            /* Dump raw bytes to diagnose — this event has come through
             * with invalid link_type values like 0xFF or 0x14 */
            snprintf(ln, sizeof(ln), "0x04 sz=%u pl=%u",
                     (unsigned)size, (unsigned)packet[1]);
            print_evt(ln);
            snprintf(ln, sizeof(ln), "%02X%02X %02X%02X %02X%02X %02X%02X",
                     packet[2], packet[3], packet[4], packet[5],
                     packet[6], packet[7], packet[8], packet[9]);
            print_evt(ln);
            break;
        case HCI_EVENT_COMMAND_STATUS:
            /* Only log non-success command statuses */
            if(packet[2] != 0) {
                /* [2]=status [3]=num_pkts [4..5]=opcode */
                uint16_t opc = packet[4] | (packet[5] << 8);
                snprintf(ln, sizeof(ln), "cmd_st=%d op=%04X",
                         packet[2], opc);
                print_evt(ln);
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            snprintf(ln, sizeof(ln), "conn st=%d", packet[2]);
            print_evt(ln);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            snprintf(ln, sizeof(ln), "dis st=%d rsn=%02x", packet[2], packet[5]);
            print_evt(ln);
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            snprintf(ln, sizeof(ln), "auth st=%d", packet[2]);
            print_evt(ln);
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            snprintf(ln, sizeof(ln), "enc st=%d on=%d", packet[2], packet[5]);
            print_evt(ln);
            break;
        case HCI_EVENT_IO_CAPABILITY_REQUEST:
            print_evt("iocap req");
            break;
        case HCI_EVENT_IO_CAPABILITY_RESPONSE:
            print_evt("iocap resp");
            break;
        case HCI_EVENT_LINK_KEY_REQUEST:
            print_evt("LK request");
            break;
        default:
            /* Silent for internal chatter: command complete/status,
             * completed packets. Only log events that suggest trouble. */
            if(ev == HCI_EVENT_COMMAND_COMPLETE) break;
            if(ev == HCI_EVENT_COMMAND_STATUS) break;
            if(ev == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS) break;
            if(ev == HCI_EVENT_TRANSPORT_PACKET_SENT) break;
            /* Silence BTstack-internal synthesized events (0x60-0x6F) —
             * SCAN_MODE_CHANGED, NR_CONNECTIONS_CHANGED, etc. are noise. */
            if(ev >= 0x60 && ev <= 0x6F) break;
            snprintf(ln, sizeof(ln), "ev 0x%02x", ev);
            print_evt(ln);
            break;
    }
}

bool dbg_bt_pair(void)
{
    s_row = 0;
    s_done = false;
    s_peer_set = false;
    s_latch[0] = '\0';

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    print_evt("BT pair");

    bt_hw_power(true);
    print_evt("power: on");

    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    uint8_t ev_[16];
    if(bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                         ev_, sizeof(ev_), 2000) < 7) {
        print_evt("rst1 fail"); goto done;
    }
    if(bt_bcm_patchram_upload(BT_BCM_FW_PATH) <= 0) {
        print_evt("patchram fail"); goto done;
    }
    mdelay(500);
    if(bt_hci_cmd_reply(hci_reset, sizeof(hci_reset),
                         ev_, sizeof(ev_), 2000) < 7) {
        print_evt("rst2 fail"); goto done;
    }
    print_evt("fw ready");

    hal_uart_dma_init();

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    static hci_transport_config_uart_t cfg = {
        .type          = HCI_TRANSPORT_CONFIG_UART,
        .baudrate_init = BT_UART_BAUD_INIT,
        .baudrate_main = 0,
        .flowcontrol   = 1,
        .device_name   = NULL,
        .parity        = 0,
    };
    hci_init(hci_transport_h4_instance(btstack_uart_block_embedded_instance()),
             &cfg);
    hci_set_link_key_db(btstack_link_key_db_memory_instance());

    /* Don't request master role on accept — iPhone refuses role switch and
     * the LMP exchange times out (Connection Accept Timeout, status 0x10). */
    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    /* L2CAP is required by SDP and any classic profile */
    l2cap_init();

    /* SDP server so iOS can query profiles after pairing (otherwise the
     * phone completes SSP then disconnects because it can't find services). */
    sdp_init();

    /* Configure device identity and SSP behavior */
    gap_set_local_name("Rockbox F20");
    gap_set_class_of_device(0x240404);  /* Audio / Wearable Headset Device */
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_set_bondable_mode(1);
    /* SC disabled — older BR/EDR devices (Bose) time out during SC negotiation
     * (LMP Response Timeout, status 0x24). Can re-enable for modern peers. */
    gap_secure_connections_enable(false);

    static btstack_packet_callback_registration_t reg;
    reg.callback = &pair_packet_handler;
    hci_add_event_handler(&reg);

    print_evt("hci_power on");
    hci_power_control(HCI_POWER_ON);

    /* Stay in pairing mode until POWER pressed, bond completes, or 120s. */
    long deadline = current_tick + 120 * HZ;
    while(!s_done && !TIME_AFTER(current_tick, deadline)) {
        btstack_run_loop_embedded_execute_once();
        if(get_action(CONTEXT_STD, 0) == ACTION_STD_CANCEL)
            break;
    }
    /* Give the stack ~1s to process the post-bond disconnect cleanly. */
    long post = current_tick + HZ;
    while(!TIME_AFTER(current_tick, post))
        btstack_run_loop_embedded_execute_once();

    hci_power_control(HCI_POWER_OFF);

done:
    bt_hw_power(false);
    print_evt("---");
    lcd_puts(0, 15, "BACK to exit");
    lcd_update();
    while(get_action(CONTEXT_STD, HZ) != ACTION_STD_CANCEL);
    return false;
}
#endif /* BOOTLOADER */
