/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Broadcom patchram firmware upload for BCM4343A1.
 *
 * Copyright (C) 2024-2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/

#include "bt-bcm-patchram.h"
#include "bt-erosqnative.h"
#include "kernel.h"
#include "file.h"

/* ---- HCI polled helpers ---- */

/* Read a complete HCI event (packet type 0x04) into buf. Returns total length
 * (including the 0x04 indicator) or -1 on protocol error / empty timeout. */
static int read_event(uint8_t* buf, int max, int first_byte_ms)
{
    if(max < 3) return -1;
    int n = bt_hw_recv(buf, 1, first_byte_ms);
    if(n != 1) return -1;
    if(buf[0] != 0x04) return 1;    /* not an event — caller can inspect */
    /* Header: event code, param length */
    if(bt_hw_recv(buf + 1, 2, 200) != 2) return -1;
    int plen = buf[2];
    int remaining = plen;
    int have = 3;
    while(remaining > 0 && have < max) {
        int take = remaining < (max - have) ? remaining : (max - have);
        int r = bt_hw_recv(buf + have, take, 200);
        if(r <= 0) break;
        have += r;
        remaining -= r;
    }
    return have;
}

int bt_hci_cmd_reply(const uint8_t* cmd, int cmd_len,
                     uint8_t* reply, int reply_max, int timeout_ms)
{
    bt_hw_drain_rx();
    if(bt_hw_send(cmd, cmd_len) != 0) return -1;
    int n = read_event(reply, reply_max, timeout_ms);
    if(n < 7 || reply[0] != 0x04 || reply[1] != 0x0E) return -1;
    return n;
}

int bt_hci_cmd(const uint8_t* cmd, int cmd_len, int timeout_ms)
{
    uint8_t reply[16];
    int n = bt_hci_cmd_reply(cmd, cmd_len, reply, sizeof(reply), timeout_ms);
    if(n < 7) return -1;
    /* reply[6] is the status byte in a Command Complete event */
    return reply[6];
}

/* ---- patchram upload ---- */

int bt_bcm_patchram_upload(const char* fwpath)
{
    int fd = open(fwpath, O_RDONLY);
    if(fd < 0) return -1;

    /* Step 1: HCI_Download_Minidriver (0xFC2E, no params) */
    static const uint8_t dl_mini[] = { 0x01, 0x2E, 0xFC, 0x00 };
    int st = bt_hci_cmd(dl_mini, sizeof(dl_mini), 500);
    if(st != 0) {
        close(fd);
        return -2;
    }
    mdelay(50);  /* chip loads minidriver */

    /* Step 2: stream each Write_RAM chunk from the .hcd file */
    uint8_t cmd[260];
    cmd[0] = 0x01;   /* HCI command packet type */
    int n_cmds = 0;
    int saw_launch = 0;
    while(1) {
        /* 3-byte HCI command header: opcode_lo, opcode_hi, plen */
        int r = read(fd, cmd + 1, 3);
        if(r == 0) break;
        if(r != 3) { close(fd); return -1; }
        int plen = cmd[3];
        if(plen > 0) {
            r = read(fd, cmd + 4, plen);
            if(r != plen) { close(fd); return -1; }
        }
        uint16_t opc = cmd[1] | (cmd[2] << 8);
        int is_launch = (opc == 0xFC4E);
        /* Launch_RAM reboots the chip; may not ack. Short timeout and don't
         * treat timeout as failure. */
        int tmo = is_launch ? 100 : 500;

        bt_hw_drain_rx();
        if(bt_hw_send(cmd, 4 + plen) != 0) { close(fd); return -4; }

        uint8_t ev[16];
        int n = read_event(ev, sizeof(ev), tmo);
        if(is_launch) {
            saw_launch = 1;
            n_cmds++;
            break;
        }
        if(n < 7 || ev[0] != 0x04 || ev[1] != 0x0E || ev[6] != 0) {
            close(fd);
            return -3;
        }
        n_cmds++;
    }
    close(fd);
    return saw_launch ? n_cmds : -3;
}
