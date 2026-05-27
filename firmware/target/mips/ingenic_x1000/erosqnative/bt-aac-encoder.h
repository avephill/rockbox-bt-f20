/***************************************************************************
 * AAC encoder interface — abstracts the choice of encoder library from
 * the BT pcm sink so we can swap backends (stub → FAAC → fdk-aac → ...)
 * without touching the A2DP send path.
 *
 * Contract:
 *   - Caller configures the encoder for a (sample_rate, channels, bit_rate)
 *     tuple obtained from AVDTP negotiation.
 *   - The encoder reports a fixed "input frame" size in stereo samples
 *     (1024 for AAC LC) and a worst-case output byte count per frame.
 *   - encode() takes exactly that many input samples and produces 0..N
 *     bytes of raw AAC LC payload (no ADTS/LATM header — A2DP spec uses
 *     raw frames in the AVDTP media payload).
 *   - encode() returns negative on hard error so the sink can fall back
 *     to silence or stop the stream cleanly.
 *
 * The "backend" is selected at compile time via BT_AAC_BACKEND:
 *   BT_AAC_BACKEND_STUB (default)  — produces zero-length output. Lets the
 *                                     rest of the code compile and the
 *                                     AVDTP layer negotiate, but no actual
 *                                     audio is encoded. Useful for testing
 *                                     the wiring before pulling in a real
 *                                     encoder.
 *   BT_AAC_BACKEND_FAAC            — FAAC (knik0/faac), plain-C LGPL
 *                                     encoder. Vendored under
 *                                     firmware/drivers/btstack/3rd-party/faac/.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Backend selector. Exactly one .c file's BT_AAC_BACKEND must match this
 * value; the others compile to nothing. Defaults to STUB so SBC users
 * pay no link cost beyond a few-byte no-op encoder. */
#define BT_AAC_BACKEND_STUB 0
#define BT_AAC_BACKEND_FAAC 1

#ifndef BT_AAC_BACKEND
#define BT_AAC_BACKEND BT_AAC_BACKEND_STUB
#endif

typedef struct bt_aac_encoder bt_aac_encoder_t;

/* Allocate + configure encoder. Returns NULL on failure (allocation, bad
 * configuration, missing backend). */
bt_aac_encoder_t* bt_aac_encoder_init(uint32_t sample_rate,
                                       uint8_t  channels,
                                       uint32_t bit_rate,
                                       bool     vbr);

/* PCM stereo samples per encode() call. For AAC LC this is fixed at 1024. */
unsigned bt_aac_encoder_input_samples(bt_aac_encoder_t* e);

/* Worst-case bytes per encoded frame. Used to size the assembly buffer. */
unsigned bt_aac_encoder_max_output(bt_aac_encoder_t* e);

/* Encode exactly bt_aac_encoder_input_samples() stereo frames from pcm
 * (interleaved L/R, int16). Returns the number of output bytes written,
 * or < 0 on error. May return 0 during encoder priming. */
int bt_aac_encoder_encode(bt_aac_encoder_t* e,
                          const int16_t* pcm_stereo,
                          unsigned       num_samples,
                          uint8_t*       out,
                          unsigned       out_max);

/* Tear down + free. */
void bt_aac_encoder_free(bt_aac_encoder_t* e);

/* Backend identifier — useful for the link-log / debug screen to indicate
 * whether the user actually has a real encoder built in. */
const char* bt_aac_encoder_backend(void);
