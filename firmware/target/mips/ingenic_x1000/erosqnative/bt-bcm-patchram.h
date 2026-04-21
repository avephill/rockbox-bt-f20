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
 * Implements the brcm_patchram_plus protocol: after an initial HCI Reset,
 * sends Download_Minidriver, streams each HCI Write_RAM chunk from a .hcd
 * file, then sends Launch_RAM to have the chip boot into full firmware.
 *
 * Copyright (C) 2024-2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/
#ifndef _BT_BCM_PATCHRAM_H
#define _BT_BCM_PATCHRAM_H

#include <stdint.h>
#include <stddef.h>

/* ---- HCI protocol helpers (polled, on top of bt_hw_send/recv) ----
 * Thin wrappers so the diag screen and patchram share one implementation. */

/* Send a command and read a Command Complete event in full.
 * `cmd` must start with the 0x01 packet indicator and be complete.
 * On success returns the total length of the event (including 0x04 indicator),
 * which will be >= 7. Return -1 on any error. */
int bt_hci_cmd_reply(const uint8_t* cmd, int cmd_len,
                     uint8_t* reply, int reply_max, int timeout_ms);

/* Send a command and return the status byte from the Command Complete event.
 * Returns 0 on success, the HCI status byte for protocol errors, or -1 on
 * transport errors. */
int bt_hci_cmd(const uint8_t* cmd, int cmd_len, int timeout_ms);

/* ---- patchram upload ---- */

/* Upload a Broadcom .hcd patchram file. The chip must already have responded
 * to an HCI Reset (ROM state). Returns the total number of commands sent
 * (including final Launch_RAM) on success, or a negative error code:
 *   -1 file open/read failure
 *   -2 vendor Download_Minidriver rejected
 *   -3 a Write_RAM command was rejected
 *   -4 transport error (UART timeout). */
int bt_bcm_patchram_upload(const char* fwpath);

#endif /* _BT_BCM_PATCHRAM_H */
