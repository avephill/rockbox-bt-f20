/***************************************************************************
 * AAC encoder — vo-aacenc (VisualOn) backend.
 *
 * Active when BT_AAC_BACKEND == BT_AAC_BACKEND_VOAAC (set via BT_AAC_USE_VOAAC
 * in the target config, after vendoring with vendor-voaac.sh).
 *
 * Why vo-aacenc on this target:
 *   - FIXED-POINT: no libm dependency (FAAC's float path needs sqrtf/powf/...
 *     which don't exist in the firmware link), and fast on the FPU-less,
 *     soft-float X1000 — no soft-float emulation in the hot path.
 *   - Caller-supplied VO_MEM_OPERATOR: no global malloc. We back it with a
 *     fixed static arena (allocations happen once at init, never in the hot
 *     path, and Free is a no-op — the whole arena is reset on encoder teardown
 *     and re-init).
 *   - adtsUsed = 0: emits raw AAC access units, which is exactly what the A2DP
 *     media payload carries (no ADTS/LATM wrapper — A2DP spec §4.5).
 *
 * The VO encode model: SetInputData(one 1024-sample/ch block) then
 * GetOutputData() yields one raw AU (VO_ERR_NONE), or VO_ERR_INPUT_BUFFER_SMALL
 * while the encoder primes (lookahead) — which we surface as 0 bytes so the
 * sink just retries next tick, matching the bt_aac_encoder contract.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef BOOTLOADER

#include "bt-aac-encoder.h"

#if BT_AAC_BACKEND == BT_AAC_BACKEND_VOAAC

#include <string.h>
#include <stdint.h>
#include "system.h"
#include "bt-link-log.h"

/* vo-aacenc public API (staged by vendor-voaac.sh under
 * 3rd-party/voaac/common/include, on the include path via firmware.make). */
#include "voAAC.h"
#include "voType.h"

/* ---- static-arena memory operator -------------------------------------
 *
 * vo-aacenc allocates its whole working set (encoder struct, MDCT/psy/quant
 * buffers) at init time via this operator. Measured high-water on hardware
 * (44.1 kHz stereo, 128 kbps) is 48928 B — see the "voaac arena N/M" log line.
 * 64 KB gives ~15 KB margin over that and keeps the static footprint modest;
 * the log line still reports the live high-water so an overflow would be
 * obvious. Bump allocator; Free is a no-op (we reset on init/teardown). */
#define VOAAC_ARENA_SIZE (64 * 1024)
static unsigned char s_arena[VOAAC_ARENA_SIZE] __attribute__((aligned(8)));
static size_t        s_arena_used;
static size_t        s_arena_hi;

static VO_U32 arena_alloc(VO_S32 uID, VO_MEM_INFO* p)
{
    (void)uID;
    size_t sz  = (size_t)p->Size;
    size_t off = (s_arena_used + 7u) & ~(size_t)7u;     /* 8-byte align */
    if (off + sz > VOAAC_ARENA_SIZE) {
        p->VBuffer = NULL;
        p->PBuffer = NULL;
        return VO_ERR_OUTOF_MEMORY;
    }
    p->VBuffer = &s_arena[off];
    p->PBuffer = p->VBuffer;
    s_arena_used = off + sz;
    if (s_arena_used > s_arena_hi) s_arena_hi = s_arena_used;
    return VO_ERR_NONE;
}
static VO_U32 arena_free(VO_S32 uID, VO_PTR p)            { (void)uID; (void)p; return VO_ERR_NONE; }
static VO_U32 arena_set(VO_S32 uID, VO_PTR p, VO_U8 v, VO_U32 n)   { (void)uID; memset(p, v, n); return VO_ERR_NONE; }
static VO_U32 arena_copy(VO_S32 uID, VO_PTR d, VO_PTR s, VO_U32 n) { (void)uID; memcpy(d, s, n); return VO_ERR_NONE; }
static VO_U32 arena_check(VO_S32 uID, VO_PTR p, VO_U32 n)          { (void)uID; (void)p; (void)n; return VO_ERR_NONE; }
static VO_S32 arena_compare(VO_S32 uID, VO_PTR a, VO_PTR b, VO_U32 n) { (void)uID; return (VO_S32)memcmp(a, b, n); }
static VO_U32 arena_move(VO_S32 uID, VO_PTR d, VO_PTR s, VO_U32 n) { (void)uID; memmove(d, s, n); return VO_ERR_NONE; }

/* ---- encoder instance -------------------------------------------------- */
struct bt_aac_encoder {
    VO_AUDIO_CODECAPI api;
    VO_HANDLE         h;
    VO_MEM_OPERATOR   moper;
    uint8_t           channels;
    unsigned          input_samples;   /* per-channel PCM samples per encode (1024 for AAC LC) */
    unsigned          max_output;
    uint8_t           freq_idx;        /* AAC sampling-frequency index for the ASC */
    uint8_t           chan_cfg;        /* AAC channelConfiguration for the ASC */
};
static struct bt_aac_encoder s_inst;
static bool                  s_in_use;

/* Scratch for the raw AU out of vo-aacenc before LATM wrapping. 1536 = the
 * encoder's max_output bound. */
static uint8_t s_raw[1536];

/* ---- LATM AudioMuxElement(muxConfigPresent=1) muxer -------------------------
 *
 * A2DP AAC to Apple H1 sinks (Beats Fit Pro, AirPods) must be LATM, NOT raw
 * access units — verified on hardware: raw AUs (correct size, correct content)
 * decode to silence. AOSP's a2dp_aac_encoder.cc uses TT_MP4_LATM_MCP1, which is
 * what every phone sends to these sinks. (The A2DP spec text says "LATM shall
 * not be used"; real Apple sinks require it anyway.)
 *
 * We emit one AudioMuxElement per RTP packet with the StreamMuxConfig present
 * every frame (useSameStreamMux=0 → stateless, decoder re-reads config each
 * packet). MSB-first bit writer; bw assumes the destination is pre-zeroed so it
 * only needs to OR in the 1-bits. */
typedef struct { uint8_t* buf; unsigned bitpos; unsigned cap; } bitw_t;

static void bw_bits(bitw_t* b, uint32_t val, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        if ((val >> i) & 1u) {
            unsigned byte = b->bitpos >> 3;
            if (byte < b->cap) b->buf[byte] |= (uint8_t)(1u << (7 - (b->bitpos & 7)));
        }
        b->bitpos++;
    }
}

/* Wrap `raw[0..raw_len)` into a LATM AudioMuxElement in `out` (pre-zeroed by
 * us). Returns the byte-aligned length, or 0 if it wouldn't fit. */
static int latm_wrap(const struct bt_aac_encoder* e, const uint8_t* raw,
                     unsigned raw_len, uint8_t* out, unsigned out_max)
{
    /* Worst-case size: header (~9 B) + length bytes + payload. Bail early. */
    if (raw_len + 16 > out_max) return 0;
    memset(out, 0, out_max);
    bitw_t b = { out, 0, out_max };

    bw_bits(&b, 0, 1);            /* useSameStreamMux = 0 -> StreamMuxConfig follows */

    /* StreamMuxConfig */
    bw_bits(&b, 0, 1);           /* audioMuxVersion = 0 */
    bw_bits(&b, 1, 1);           /* allStreamsSameTimeFraming = 1 */
    bw_bits(&b, 0, 6);           /* numSubFrames = 0 (one) */
    bw_bits(&b, 0, 4);           /* numProgram = 0 (one) */
    bw_bits(&b, 0, 3);           /* numLayer = 0 (one) */
    /* AudioSpecificConfig (AAC LC) — prog0/lay0 implies useSameConfig=0 */
    bw_bits(&b, 2, 5);           /* audioObjectType = 2 (AAC LC) */
    bw_bits(&b, e->freq_idx, 4); /* samplingFrequencyIndex */
    bw_bits(&b, e->chan_cfg, 4); /* channelConfiguration */
    bw_bits(&b, 0, 1);           /* GASpecificConfig: frameLengthFlag = 0 (1024) */
    bw_bits(&b, 0, 1);           /* dependsOnCoreCoder = 0 */
    bw_bits(&b, 0, 1);           /* extensionFlag = 0 */
    bw_bits(&b, 0, 3);           /* frameLengthType = 0 */
    bw_bits(&b, 0xFF, 8);        /* latmBufferFullness = 0xFF (no info) */
    bw_bits(&b, 0, 1);           /* otherDataPresent = 0 */
    bw_bits(&b, 0, 1);           /* crcCheckPresent = 0 */

    /* PayloadLengthInfo: MuxSlotLengthBytes for frameLengthType 0 */
    unsigned L = raw_len;
    while (L >= 255) { bw_bits(&b, 255, 8); L -= 255; }
    bw_bits(&b, L, 8);

    /* PayloadMux: the raw AU bytes (bit-shifted by the writer) */
    for (unsigned i = 0; i < raw_len; i++) bw_bits(&b, raw[i], 8);

    /* ByteAlign() */
    return (int)((b.bitpos + 7) >> 3);
}

bt_aac_encoder_t* bt_aac_encoder_init(uint32_t sample_rate, uint8_t channels,
                                       uint32_t bit_rate, bool vbr)
{
    (void)vbr;   /* vo-aacenc is CBR; bit_rate is the AVDTP-negotiated aggregate */
    if (s_in_use) return NULL;
    memset(&s_inst, 0, sizeof(s_inst));
    s_arena_used = 0;
    s_arena_hi   = 0;

    s_inst.moper.Alloc   = arena_alloc;
    s_inst.moper.Free    = arena_free;
    s_inst.moper.Set     = arena_set;
    s_inst.moper.Copy    = arena_copy;
    s_inst.moper.Check   = arena_check;
    s_inst.moper.Compare = arena_compare;
    s_inst.moper.Move    = arena_move;

    if (voGetAACEncAPI(&s_inst.api) != VO_ERR_NONE) return NULL;

    VO_CODEC_INIT_USERDATA ud;
    memset(&ud, 0, sizeof(ud));
    ud.memflag = VO_IMF_USERMEMOPERATOR;
    ud.memData = (VO_PTR)&s_inst.moper;

    if (s_inst.api.Init(&s_inst.h, VO_AUDIO_CodingAAC, &ud) != VO_ERR_NONE)
        return NULL;

    AACENC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.sampleRate = (int)sample_rate;
    p.bitRate    = (int)bit_rate;
    p.nChannels  = (short)channels;
    p.adtsUsed   = 0;                  /* raw AAC AUs for A2DP */
    if (s_inst.api.SetParam(s_inst.h, VO_PID_AAC_ENCPARAM, &p) != VO_ERR_NONE) {
        s_inst.api.Uninit(s_inst.h);
        s_inst.h = NULL;
        return NULL;
    }

    s_inst.channels      = channels;
    s_inst.input_samples = 1024;       /* AAC LC, per channel */
    s_inst.max_output    = 1536;       /* generous raw-AU bound (~768 B/ch) */

    /* ASC fields for LATM wrapping. */
    static const uint32_t sf[] = { 96000,88200,64000,48000,44100,32000,24000,
                                   22050,16000,12000,11025,8000,7350 };
    s_inst.freq_idx = 4;   /* default 44100 */
    for (unsigned i = 0; i < sizeof(sf)/sizeof(sf[0]); i++)
        if (sf[i] == sample_rate) { s_inst.freq_idx = (uint8_t)i; break; }
    s_inst.chan_cfg = channels;   /* 1=mono, 2=stereo */
    s_in_use = true;

    /* All vo-aacenc allocations are done by now (Init + SetParam). Log the
     * arena high-water so the size can be verified/tuned from the link log. */
    bt_link_logf("voaac arena %u/%u",
                 (unsigned)s_arena_hi, (unsigned)VOAAC_ARENA_SIZE);
    return &s_inst;
}

unsigned bt_aac_encoder_input_samples(bt_aac_encoder_t* e) { return e->input_samples; }
unsigned bt_aac_encoder_max_output(bt_aac_encoder_t* e)    { return e->max_output; }

int bt_aac_encoder_encode(bt_aac_encoder_t* e,
                          const int16_t* pcm_stereo,
                          unsigned num_samples,
                          uint8_t* out, unsigned out_max)
{
    VO_CODECBUFFER in;
    memset(&in, 0, sizeof(in));
    /* VO API isn't const-correct; it does not mutate the input. */
    in.Buffer = (VO_PBYTE)(void*)(uintptr_t)pcm_stereo;
    in.Length = (VO_U32)(num_samples * e->channels * sizeof(int16_t));
    if (e->api.SetInputData(e->h, &in) != VO_ERR_NONE) return -1;

    /* Encode into scratch, then LATM-wrap into the caller's buffer (A2DP AAC
     * to Apple sinks requires LATM, not raw AUs — see latm_wrap comment). */
    VO_CODECBUFFER outb;
    memset(&outb, 0, sizeof(outb));
    outb.Buffer = s_raw;
    outb.Length = sizeof(s_raw);
    VO_AUDIO_OUTPUTINFO info;
    memset(&info, 0, sizeof(info));

    VO_U32 rc = e->api.GetOutputData(e->h, &outb, &info);
    if (rc == VO_ERR_INPUT_BUFFER_SMALL) return 0;   /* priming / need more */
    if (rc != VO_ERR_NONE)               return -1;
    if (outb.Length == 0)                return 0;
    return latm_wrap(e, s_raw, outb.Length, out, out_max);  /* 0 if it won't fit */
}

void bt_aac_encoder_free(bt_aac_encoder_t* e)
{
    if (e != &s_inst) return;
    if (s_inst.h) {
        s_inst.api.Uninit(s_inst.h);
        s_inst.h = NULL;
    }
    s_arena_used = 0;
    s_in_use = false;
}

const char* bt_aac_encoder_backend(void) { return "vo-aacenc"; }

#endif /* BT_AAC_BACKEND == BT_AAC_BACKEND_VOAAC */
#endif /* !BOOTLOADER */
