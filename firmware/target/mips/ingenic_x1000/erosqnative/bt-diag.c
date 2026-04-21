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
    lcd_puts(0, 15, "POWER to exit");
    lcd_update();
    while(get_action(CONTEXT_STD, HZ) != ACTION_STD_CANCEL);
    return false;
}
#endif /* BOOTLOADER */
