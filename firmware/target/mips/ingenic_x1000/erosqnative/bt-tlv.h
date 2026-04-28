/***************************************************************************
 * BT TLV — file-backed Tag-Value-Length store for persistent link keys.
 *
 * Wraps a single fixed-size file at /.rockbox/bt_keys.dat as a small
 * key/value table. BTstack's btstack_link_key_db_tlv adapter calls into
 * this through the standard btstack_tlv_t interface.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

#include "btstack_tlv.h"

/* Initialize the TLV store. Loads the on-disk file into the in-memory
 * cache; safe to call before the file exists (cache starts empty). */
void bt_tlv_init(void);

/* btstack_tlv_t interface for use with btstack_link_key_db_tlv. */
extern const btstack_tlv_t bt_tlv_impl;
