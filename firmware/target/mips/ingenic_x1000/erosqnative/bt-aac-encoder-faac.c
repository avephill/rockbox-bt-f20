/***************************************************************************
 * AAC encoder — FAAC backend.
 *
 * Wraps knik0/faac (plain C, LGPL 2.1+, ~270 KB) as the bt_aac_encoder_t
 * implementation. Active when BT_AAC_BACKEND == BT_AAC_BACKEND_FAAC is
 * passed on the firmware build command line (and the FAAC source has been
 * vendored — see firmware/drivers/btstack/3rd-party/faac/ and the
 * vendor-faac.sh helper next to this file).
 *
 * Configuration we ask FAAC for:
 *   - MPEG-4, AAC LC (object type LOW)
 *   - Raw bitstream (no ADTS/LATM wrapper — A2DP packs raw frames in the
 *     AVDTP media payload, see A2DP v1.3.2 section 4.5)
 *   - Input format: native-endian 16-bit (the same int16 we already produce
 *     from pull_pcm). FAAC's input buffer is typed int32_t* for alignment
 *     but the actual element values are int16 in 16BIT mode.
 *   - Bit rate is the AVDTP-negotiated value, split across channels for
 *     FAAC's per-channel bitRate field.
 *
 * Performance caveat
 * ------------------
 * FAAC is floating-point. The X1000 SoC has no FPU, so every sample
 * runs through GCC's soft-float library. AAC LC at 1024-sample frames @
 * 44.1 kHz means we have ~23 ms of CPU budget per frame to keep up; on
 * a 1 GHz MIPS32 with no FPU this is *tight*, and may not work at all
 * without further optimization (fixed-point port, IRAM-resident inner
 * loops, etc.). We won't know until we measure. If audio underruns,
 * fall back to SBC by rebuilding without BT_AAC_BACKEND=FAAC.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef BOOTLOADER

#include "bt-aac-encoder.h"

#if BT_AAC_BACKEND == BT_AAC_BACKEND_FAAC

#include <string.h>
#include <stdlib.h>
#include "system.h"

/* FAAC headers — supplied by the vendoring step. The include path is
 * set up by firmware.make once the 3rd-party/faac directory is populated. */
#include "faac.h"

struct bt_aac_encoder {
    faacEncHandle handle;
    unsigned      input_samples;     /* PCM samples per encode call (per FAAC: total samples = 1024 * channels) */
    unsigned      max_output;        /* worst-case output bytes per call */
    uint8_t       channels;
};

/* Single static instance — see comment in bt-aac-encoder-stub.c. */
static struct bt_aac_encoder s_inst;
static bool                  s_in_use;

bt_aac_encoder_t* bt_aac_encoder_init(uint32_t sample_rate, uint8_t channels,
                                       uint32_t bit_rate, bool vbr)
{
    if(s_in_use) return NULL;
    memset(&s_inst, 0, sizeof(s_inst));

    unsigned long in_samples = 0;
    unsigned long max_out    = 0;
    faacEncHandle h = faacEncOpen(sample_rate, channels, &in_samples, &max_out);
    if(!h) return NULL;

    faacEncConfigurationPtr cfg = faacEncGetCurrentConfiguration(h);
    if(!cfg) { faacEncClose(h); return NULL; }

    /* AAC LC (object type LOW = 2), MPEG-4, raw bitstream */
    cfg->aacObjectType = 2;          /* LOW */
    cfg->mpegVersion   = 4;
    cfg->outputFormat  = 0;          /* raw — A2DP wants no ADTS/LATM */
    cfg->inputFormat   = 1;          /* FAAC_INPUT_16BIT */
    cfg->useTns        = 1;
    cfg->allowMidside  = 1;
    cfg->bandWidth     = 0;          /* let FAAC pick from bit rate */
    /* FAAC bitRate is per channel. AVDTP gives us the aggregate the sink
     * picked. VBR isn't a direct FAAC knob — high quantizer-quality + the
     * fixed bitRate gives quasi-VBR. We honour the vbr flag mainly for
     * future use; FAAC will not strictly conform but produces a similar
     * frame-size distribution. */
    cfg->bitRate       = bit_rate / (channels > 0 ? channels : 1);
    (void)vbr;

    if(!faacEncSetConfiguration(h, cfg)) {
        faacEncClose(h);
        return NULL;
    }

    s_inst.handle        = h;
    s_inst.input_samples = (unsigned)in_samples;
    s_inst.max_output    = (unsigned)max_out;
    s_inst.channels      = channels;
    s_in_use = true;
    return &s_inst;
}

unsigned bt_aac_encoder_input_samples(bt_aac_encoder_t* e)
{
    /* FAAC reports total interleaved samples (e.g. 2048 for stereo). The
     * sink interface speaks stereo frames (i.e. 1024 for stereo @ AAC LC).
     * For our 2-channel use this is in_samples/2. Guard against integer
     * surprises if anyone ever runs us mono. */
    return e->input_samples / (e->channels > 0 ? e->channels : 1);
}

unsigned bt_aac_encoder_max_output(bt_aac_encoder_t* e)
{
    return e->max_output;
}

int bt_aac_encoder_encode(bt_aac_encoder_t* e,
                          const int16_t* pcm_stereo,
                          unsigned num_samples,
                          uint8_t* out, unsigned out_max)
{
    /* FAAC wants samples_input as the TOTAL count (1024 * channels). */
    unsigned total = num_samples * e->channels;
    /* FAAC's prototype takes int32_t* even when inputFormat is 16BIT —
     * the bytes are read as int16_t pairs. Cast the const away because
     * the FAAC API isn't const-correct; it does not mutate the input. */
    int n = faacEncEncode(e->handle,
                          (int32_t*)(void*)(uintptr_t)pcm_stereo,
                          total,
                          out, out_max);
    return n;     /* >0 = bytes; 0 = priming; <0 = error */
}

void bt_aac_encoder_free(bt_aac_encoder_t* e)
{
    if(e != &s_inst) return;
    if(s_inst.handle) {
        faacEncClose(s_inst.handle);
        s_inst.handle = NULL;
    }
    s_in_use = false;
}

const char* bt_aac_encoder_backend(void) { return "faac"; }

#endif /* BT_AAC_BACKEND == BT_AAC_BACKEND_FAAC */
#endif /* !BOOTLOADER */
