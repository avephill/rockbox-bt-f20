/***************************************************************************
 * BT TLV — file-backed Tag-Value-Length store for persistent link keys.
 *
 * Implements BTstack's btstack_tlv_t interface against a single fixed-size
 * file at /.rockbox/bt_keys.dat. Used by btstack_link_key_db_tlv so paired
 * devices survive reboots (today the in-memory link_key_db forced a
 * re-pair on every boot; see roadmap C7 in project-status.md).
 *
 * Layout: a 16-byte header followed by NVM_NUM_LINK_KEYS fixed-size slots.
 * The whole file is cached in RAM; mutations rewrite the file atomically
 * via tmpfile + rename. With NVM_NUM_LINK_KEYS=8 and 32-byte payloads the
 * file is ~320 bytes — small enough that whole-file rewrites are cheap.
 *
 * The link_key_db_tlv adapter stores 27-byte records (link_key_nvm_t),
 * so BT_TLV_DATA_MAX=32 leaves headroom. If the layout ever needs to
 * grow beyond 32 bytes, bump BT_TLV_DATA_MAX and the on-disk magic so
 * old files are discarded rather than mis-decoded.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/

#ifndef BOOTLOADER

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "file.h"
#include "rbpaths.h"

#include "bt-tlv.h"

#define BT_TLV_PATH        ROCKBOX_DIR "/bt_keys.dat"
#define BT_TLV_PATH_TMP    ROCKBOX_DIR "/bt_keys.tmp"
#define BT_TLV_MAGIC       0x4254565F   /* "BTV_" */
#define BT_TLV_VERSION     1
#define BT_TLV_DATA_MAX    32
#define BT_TLV_NUM_SLOTS   8            /* matches NVM_NUM_LINK_KEYS */

struct bt_tlv_slot {
    uint32_t tag;       /* 0 = empty */
    uint16_t len;
    uint16_t pad;
    uint8_t  data[BT_TLV_DATA_MAX];
};

struct bt_tlv_file {
    uint32_t magic;
    uint32_t version;
    uint32_t num_slots;     /* expected to equal BT_TLV_NUM_SLOTS */
    uint32_t reserved;
    struct bt_tlv_slot slots[BT_TLV_NUM_SLOTS];
};

static struct bt_tlv_file s_cache;
static bool               s_loaded;

static void bt_tlv_reset_cache(void)
{
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.magic     = BT_TLV_MAGIC;
    s_cache.version   = BT_TLV_VERSION;
    s_cache.num_slots = BT_TLV_NUM_SLOTS;
}

static void bt_tlv_load(void)
{
    bt_tlv_reset_cache();
    int fd = open(BT_TLV_PATH, O_RDONLY);
    if(fd < 0) return;
    struct bt_tlv_file f;
    int n = read(fd, &f, sizeof(f));
    close(fd);
    /* Reject files that don't match our exact layout — discarding them is
     * preferable to mis-decoding. The user just re-pairs once. */
    if(n != (int)sizeof(f)
       || f.magic != BT_TLV_MAGIC
       || f.version != BT_TLV_VERSION
       || f.num_slots != BT_TLV_NUM_SLOTS) {
        return;
    }
    s_cache = f;
}

static void bt_tlv_save(void)
{
    /* Atomic rewrite: write tmp file then rename over the live file. On
     * FAT this is effectively atomic from the reader's perspective. */
    int fd = open(BT_TLV_PATH_TMP, O_CREAT|O_WRONLY|O_TRUNC, 0666);
    if(fd < 0) return;
    int n = write(fd, &s_cache, sizeof(s_cache));
    close(fd);
    if(n != (int)sizeof(s_cache)) {
        remove(BT_TLV_PATH_TMP);
        return;
    }
    remove(BT_TLV_PATH);   /* rename() over an existing file may fail on FAT */
    rename(BT_TLV_PATH_TMP, BT_TLV_PATH);
}

void bt_tlv_init(void)
{
    if(s_loaded) return;
    bt_tlv_load();
    s_loaded = true;
}

static struct bt_tlv_slot* find_slot(uint32_t tag)
{
    if(tag == 0) return NULL;
    for(int i = 0; i < BT_TLV_NUM_SLOTS; i++)
        if(s_cache.slots[i].tag == tag) return &s_cache.slots[i];
    return NULL;
}

static struct bt_tlv_slot* find_empty_slot(void)
{
    for(int i = 0; i < BT_TLV_NUM_SLOTS; i++)
        if(s_cache.slots[i].tag == 0) return &s_cache.slots[i];
    return NULL;
}

static int impl_get_tag(void* ctx, uint32_t tag, uint8_t* buf, uint32_t bufsz)
{
    (void)ctx;
    struct bt_tlv_slot* s = find_slot(tag);
    if(!s) return 0;
    uint32_t n = s->len < bufsz ? s->len : bufsz;
    memcpy(buf, s->data, n);
    return (int)s->len;
}

static int impl_store_tag(void* ctx, uint32_t tag, const uint8_t* data, uint32_t len)
{
    (void)ctx;
    if(tag == 0 || len > BT_TLV_DATA_MAX) return -1;
    struct bt_tlv_slot* s = find_slot(tag);
    if(!s) s = find_empty_slot();
    if(!s) {
        /* Full — evict slot 0 (the link_key_db_tlv adapter does its own
         * least-recently-used eviction via a seq number in the value, so
         * arriving here means we've also exhausted that). */
        s = &s_cache.slots[0];
    }
    s->tag = tag;
    s->len = (uint16_t)len;
    memset(s->data, 0, sizeof(s->data));
    memcpy(s->data, data, len);
    bt_tlv_save();
    return 0;
}

static void impl_delete_tag(void* ctx, uint32_t tag)
{
    (void)ctx;
    struct bt_tlv_slot* s = find_slot(tag);
    if(!s) return;
    s->tag = 0;
    s->len = 0;
    memset(s->data, 0, sizeof(s->data));
    bt_tlv_save();
}

const btstack_tlv_t bt_tlv_impl = {
    .get_tag    = impl_get_tag,
    .store_tag  = impl_store_tag,
    .delete_tag = impl_delete_tag,
};

#endif /* !BOOTLOADER */
