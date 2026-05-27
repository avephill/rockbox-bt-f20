/***************************************************************************
 * AAC encoder — stub backend (silence).
 *
 * Always compiled. Active when BT_AAC_BACKEND is not defined or set to
 * BT_AAC_BACKEND_STUB. Lets the rest of the AAC code compile, AVDTP
 * negotiate, and the run-time path exercise without requiring a real
 * encoder library. Produces zero-length output (sink-side will hear
 * silence / underrun).
 *
 * The whole point of having this is so the AAC code path can be turned
 * on for protocol-side testing (does BFP negotiate AAC? what bit rate /
 * sample rate does it pick? does AVRCP keep working?) without coupling
 * to the encoder vendoring effort.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef BOOTLOADER

#include "bt-aac-encoder.h"

#if BT_AAC_BACKEND == BT_AAC_BACKEND_STUB

#include <string.h>
#include "system.h"

struct bt_aac_encoder {
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t bit_rate;
    bool     vbr;
};

/* Single static instance — we only ever stream to one sink, no point
 * dynamically allocating. Matches the SBC encoder pattern (s_sbc_state
 * in bt-pcm-sink.c). */
static struct bt_aac_encoder s_inst;
static bool                  s_in_use;

bt_aac_encoder_t* bt_aac_encoder_init(uint32_t sample_rate, uint8_t channels,
                                       uint32_t bit_rate, bool vbr)
{
    if(s_in_use) return NULL;
    memset(&s_inst, 0, sizeof(s_inst));
    s_inst.sample_rate = sample_rate;
    s_inst.channels    = channels;
    s_inst.bit_rate    = bit_rate;
    s_inst.vbr         = vbr;
    s_in_use = true;
    return &s_inst;
}

unsigned bt_aac_encoder_input_samples(bt_aac_encoder_t* e)
{
    (void)e;
    /* AAC LC fundamental frame size, regardless of sample rate. */
    return 1024;
}

unsigned bt_aac_encoder_max_output(bt_aac_encoder_t* e)
{
    (void)e;
    /* AAC LC worst-case frame is ~768 bytes per channel at high bit
     * rates; we pad to 1024 to give the eventual real encoder room. */
    return 1024;
}

int bt_aac_encoder_encode(bt_aac_encoder_t* e,
                          const int16_t* pcm_stereo,
                          unsigned num_samples,
                          uint8_t* out, unsigned out_max)
{
    (void)e; (void)pcm_stereo; (void)num_samples; (void)out; (void)out_max;
    /* Silence — no actual bitstream produced. Caller will see this as a
     * media packet with zero bytes of AAC payload. */
    return 0;
}

void bt_aac_encoder_free(bt_aac_encoder_t* e)
{
    if(e == &s_inst) s_in_use = false;
}

const char* bt_aac_encoder_backend(void) { return "stub"; }

#endif /* BT_AAC_BACKEND == BT_AAC_BACKEND_STUB */
#endif /* !BOOTLOADER */
