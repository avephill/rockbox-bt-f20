/***************************************************************************
 * BT PCM sink — bridges Rockbox PCM playback to BTstack A2DP source.
 *
 * Owns the SBC encoder and the audio-rate pacing timer. Implements the
 * pcm_sink_t interface so it can be swapped in via pcm_set_current_sink().
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Active codec. Selected by bt-service on AVDTP MEDIA_CODEC_*_CONFIGURATION,
 * which arrives just before STREAM_ESTABLISHED. The set_*_config calls
 * implicitly latch the codec. */
enum bt_pcm_codec {
    BT_PCM_CODEC_SBC = 0,
    BT_PCM_CODEC_AAC,
};

/* Configure the SBC encoder with parameters negotiated by AVDTP. Call on
 * A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION. Latches codec=SBC. */
void bt_pcm_sink_set_sbc_config(uint16_t freq, uint8_t block_length,
                                 uint8_t subbands, uint8_t alloc, uint8_t chmode,
                                 uint8_t max_bitpool);

/* Configure the AAC encoder. Call on AAC MEDIA_CODEC configuration event.
 * Latches codec=AAC. The active encoder backend is whichever was selected
 * at compile time via BT_AAC_BACKEND in bt-aac-encoder.h (default = stub,
 * which produces silence — see comments there). */
void bt_pcm_sink_set_aac_config(uint32_t sample_rate, uint8_t channels,
                                 uint32_t bit_rate, bool vbr);

/* Active codec (for status/diag). */
enum bt_pcm_codec bt_pcm_sink_get_codec(void);

/* Start the pacing timer and arm the encoder pipeline. Call on
 * A2DP_SUBEVENT_STREAM_STARTED. */
void bt_pcm_sink_start_streaming(uint16_t a2dp_cid, uint8_t local_seid);

/* Halt the pacing timer. Sink remains installed; play() will pad silence
 * until streaming is restarted or the sink is swapped out. */
void bt_pcm_sink_stop_streaming(void);

/* Send the current media packet. Call on A2DP_SUBEVENT_STREAMING_CAN_SEND
 * _MEDIA_PACKET_NOW. */
void bt_pcm_sink_handle_can_send_now(void);

/* Diagnostic: total RTP packets sent since last start. */
uint32_t bt_pcm_sink_packets_sent(void);

/* True while bt_pcm_sink is the active output. Used by jack-detect plumbing
 * (`headphones_inserted()` in button-erosqnative.c) to keep the playback
 * engine from auto-pausing when nothing is plugged into the analog jack. */
bool bt_pcm_sink_is_active(void);
