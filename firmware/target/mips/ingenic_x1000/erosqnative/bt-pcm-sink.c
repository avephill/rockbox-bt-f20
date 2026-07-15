/***************************************************************************
 * BT PCM sink — see bt-pcm-sink.h.
 *
 * Architecture:
 *   - Implements pcm_sink_t. Upper layer (mixer/playback) submits buffers
 *     via sink_play(addr, size); we record the pointer and consume samples
 *     at SBC-rate via the BTstack run-loop timer (audio_tick).
 *   - When the current buffer is drained, pull_pcm calls
 *     pcm_play_dma_complete_callback() to fetch the next buffer from the
 *     upper layer. Underrun → pad with zero samples.
 *
 * Copyright (C) 2026 - GPLv2
 ****************************************************************************/
#ifndef BOOTLOADER

#include <string.h>
#include <stdbool.h>
#include "system.h"
#include "kernel.h"
#include "mutex.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "bt-pcm-sink.h"

#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "classic/a2dp.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "bt-link-log.h"
#include "bt-aac-encoder.h"

/* Native sample type from Rockbox playback. F20 is PCM_NATIVE_BITDEPTH=24
 * meaning 24-bit signed values stored LSB-aligned in 32-bit containers
 * (top 8 bits are sign extension). 8 bytes per stereo frame. SBC encoder
 * needs int16_t input — pull_pcm shifts right by (BITDEPTH - 16). */
#if (PCM_NATIVE_BITDEPTH > 16)
typedef int32_t pcm_sample_t;
#define PCM_TO_S16_SHIFT (PCM_NATIVE_BITDEPTH - 16)
#else
typedef int16_t pcm_sample_t;
#define PCM_TO_S16_SHIFT 0
#endif
#define PCM_FRAME_BYTES  (2 * sizeof(pcm_sample_t))

/* ---- sink state (current playback buffer) ---- */
/* Serializes BT-thread reads of pcm_sw_volume's globals (via complete
 * callback + STARTED in pull_pcm) against playback-thread writes (via
 * pcm_play_data → start_pcm). Without this, a track change crashes
 * pcm_scale_buffer_cut on a torn source pointer. The X1000 sink doesn't
 * need a real mutex because its complete-callback runs in ISR context
 * and is naturally serialized by IRQ disable inside pcm_play_lock; ours
 * runs in BT thread context and isn't. */
static struct mutex         s_pcm_lock;
static bool                 s_pcm_lock_inited;
static const pcm_sample_t*  s_buf;
static size_t               s_buf_frames;   /* total stereo frames in buf */
static size_t               s_buf_pos;      /* frames consumed */

/* ---- streaming state ---- */
static bool     s_streaming;
static bool     s_active;       /* true between start/stop_streaming */
static uint16_t s_a2dp_cid;
static uint8_t  s_local_seid;

/* Active codec — set by bt_pcm_sink_set_*_config from bt-service when
 * the AVDTP layer notifies which codec the sink picked. Drives all the
 * dispatch in audio_tick / handle_can_send_now / send_media_packet. */
static enum bt_pcm_codec s_codec = BT_PCM_CODEC_SBC;

/* Negotiated sample rate, in Hz. Used to convert wall-clock ms into a
 * sample-count budget for audio_tick. SBC and AAC both populate this. */
static uint32_t s_sample_rate;

/* ---- SBC encoder ---- */
static const btstack_sbc_encoder_t* s_sbc_encoder;
static btstack_sbc_encoder_bluedroid_t s_sbc_state;
static struct {
    uint16_t freq;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  max_bitpool;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} s_sbc_cfg;

/* ---- packet assembly (SBC: aggregate N frames per packet) ---- */
#define SBC_STORAGE_SIZE 1030
/* The A2DP SBC media payload header (s_sbc_storage[0]) carries the frame
 * count in its LOW NIBBLE only (bits 0-3); bits 4-7 are the F/S/L/RFA
 * fragmentation flags. A non-fragmented packet thus holds at most 15 frames
 * — pack more and num_frames spills into the flag bits, corrupting the
 * header so the sink drops/garbles the whole packet (audible as total
 * silence). At bitpool 35 frames are ~83 B so a ~672 B payload never reaches
 * 15 frames and the bug hides; at low bitpool (16, ~44 B/frame) we'd pack
 * 20+ frames. Cap the assembly loop at this many frames per packet.
 *
 * (A packetization experiment — capping at 6 frames, ~17 ms/packet — was
 * tried 2026-05-28 to test whether smaller packets shrink the audible stall
 * holes. It did NOT help: the dropout equals the controller's wall-clock
 * retransmit-stall duration, independent of how the audio is packetized.
 * Reverted to 15 = the header's hard limit. See the session-2 notes in
 * project-status.md.) */
#define SBC_MAX_FRAMES_PER_PACKET 15
static uint8_t  s_sbc_storage[SBC_STORAGE_SIZE];
static int      s_sbc_storage_count;
static int      s_sbc_ready_to_send;
static int      s_max_payload;
static bool     s_sbc_logged;   /* one-shot: log real frame size / count once per stream */

/* ---- AAC encoder ----
 *
 * AAC LC always consumes 1024 stereo samples per frame. The encoder backend
 * returns one LATM AudioMuxElement per frame (A2DP AAC to Apple sinks must be
 * LATM, not raw AUs — see bt-aac-encoder-voaac.c). We send one element per RTP
 * packet; there's no SBC-style per-frame header byte.
 *
 * AAC_PAYLOAD_SIZE budgets a generous 1 KB — actual AAC LC stereo at
 * <=256 kbps tops out around 700 bytes/frame in practice. The eventual
 * real encoder must respect bt_aac_encoder_max_output() (which this
 * code sizes against on start_streaming). */
#define AAC_PAYLOAD_SIZE 1024
static bt_aac_encoder_t* s_aac_enc;
static unsigned          s_aac_input_samples;   /* PCM samples per encode call */
static unsigned          s_aac_max_output;
static uint8_t           s_aac_payload[AAC_PAYLOAD_SIZE];
static int               s_aac_payload_size;    /* bytes pending send (0 = empty) */
static int               s_aac_ready_to_send;
/* Diagnostics: confirm AU wire-format (raw AAC CPE starts ~0x21; ADTS starts
 * 0xFF) and the realtime send rate (need ~43 pkt/s = ~86/2s; far less = the
 * sink starves to silence). One-shot AU dump + a 2 s throughput window. */
static bool              s_aac_logged_au;
static uint32_t          s_aac_win_start;
static uint32_t          s_aac_win_packets;
static uint32_t          s_aac_win_bytes;   /* sum of AU sizes this window */
static int               s_aac_last_size;   /* last AU byte count */
static struct {
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t bit_rate;
    bool     vbr;
} s_aac_cfg;

/* ---- audio pacing ----
 *
 * The canonical BTstack demo uses AUDIO_TIMEOUT_MS=10 and relies on the
 * timer firing at ~100 Hz to send one packet per tick. Our hal_time_ms
 * has 10 ms resolution (current_tick * 10 with HZ=100), so a 10 ms timer
 * actually re-arms at the *next* boundary, giving only ~50 Hz. With a
 * 1 ms timer the re-arm always falls in the next 10 ms boundary, so we
 * fire at the full ~100 Hz, which at ~5 SBC frames/packet * 128 samples
 * gives ~64 k samples/s — comfortably above the 44.1 k we need. */
static btstack_timer_source_t s_audio_timer;
#define AUDIO_TIMEOUT_MS 1
static uint32_t s_time_sent_ms;
static uint32_t s_acc_missed;
static uint32_t s_samples_ready;
static uint32_t s_rtp_ts;
static uint32_t s_packets_sent;
/* Time of last successful media-packet send (BTstack run-loop ms). Used to
 * detect "send stretches" — gaps when the controller-side ACL queue is
 * stalled (typically retransmit cascades). A healthy session sends one
 * packet every ~10 ms; sustained gaps > 50 ms correlate strongly with
 * audible glitches/cutouts and tell us this isn't a host-side or sink-side
 * issue but a controller/RF one. Logged via bt-link-log. */
static uint32_t s_last_send_ms;

/* Pull `num_frames` of stereo PCM into `pcm` from the current sink buffer.
 * Fetches the next buffer from the upper layer when exhausted. Pads with
 * silence on underrun (no buffer available).
 *
 * Called from BTstack run-loop context (single-threaded with the rest of
 * this file's hot path). sink_play() may run in another context — the
 * disable_irq_save() in lock/unlock and play/stop guards the pointer flip.
 */
static int pull_pcm(int16_t* pcm, int num_frames)
{
    int written = 0;
    while (written < num_frames) {
        if (!s_buf || s_buf_pos >= s_buf_frames) {
            const void* next = NULL;
            size_t nsz = 0;
            /* complete_callback + STARTED both touch pcm_sw_volume.c's
             * src_buf_addr / src_buf_rem / pcm_dbl_buf_num. Take the same
             * lock pcm_play_data holds via sink_lock so a concurrent track
             * change can't tear the source pointer mid-pull. */
            mutex_lock(&s_pcm_lock);
            /* If the upper layer has stopped pcm playback (track change,
             * underrun, manual stop), pcm_play_dma_stop_int cleared
             * src_buf_addr to NULL but left pcm_dbl_buf_size[] non-zero,
             * so complete_callback would still return true with a stale
             * buffer — and the next STARTED would compute
             * src_buf_addr + pcm_dbl_buf_size[num]/X = 0 + ~1024 = 0x400
             * as the source pointer, crashing pcm_scale_buffer_cut. Skip
             * the call entirely; the silence-pad path below handles the
             * gap, and playback will resume when pcm_play_data restarts. */
            bool ok = pcm_is_playing()
                   && pcm_play_dma_complete_callback(PCM_DMAST_OK, &next, &nsz);
            if (ok) {
                s_buf        = (const pcm_sample_t*)next;
                s_buf_frames = nsz / PCM_FRAME_BYTES;
                s_buf_pos    = 0;
                /* Mirror what X1000's play_dma_handle_event does on every
                 * buffer transition: fire STARTED so pcm_sw_volume.c refills
                 * the inactive double-buffer slot. Without this, the same
                 * buffer comes back from complete_callback indefinitely. */
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            }
            mutex_unlock(&s_pcm_lock);
            if (!ok) {
                s_buf = NULL;
                break;
            }
        }
        size_t avail = s_buf_frames - s_buf_pos;
        size_t take  = (size_t)(num_frames - written) < avail
                           ? (size_t)(num_frames - written) : avail;
        const pcm_sample_t* src = s_buf + s_buf_pos * 2;
        for (size_t i = 0; i < take; i++) {
            pcm[(written + i)*2]     = (int16_t)(src[i*2]     >> PCM_TO_S16_SHIFT);
            pcm[(written + i)*2 + 1] = (int16_t)(src[i*2 + 1] >> PCM_TO_S16_SHIFT);
        }
        s_buf_pos += take;
        written   += take;
    }
    for (int i = written; i < num_frames; i++) {
        pcm[i*2]     = 0;
        pcm[i*2 + 1] = 0;
    }
    return written;
}

static void fill_sbc(void)
{
    unsigned num_audio_samples = s_sbc_encoder->num_audio_frames(&s_sbc_state);
    if (num_audio_samples == 0) return;
    /* sbc_buffer_length() reports u16PacketLength, which SBC_Encoder only
     * fills AFTER the first encode of a stream — configure() leaves it 0.
     * So we can't divide by it (or even trust the room check) until at least
     * one frame has been encoded. Let the first frame through unconditionally,
     * then refresh the size and use it for the room + 15-frame-cap checks.
     * (The cap matters because the SBC payload header's frame count is a
     * 4-bit field — see SBC_MAX_FRAMES_PER_PACKET. The first frame always
     * fits: worst-case ~115 B vs the 1030 B storage.) */
    uint16_t sbc_frame_size = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
    while (s_samples_ready >= num_audio_samples
           && (sbc_frame_size == 0
               || ((s_max_payload - s_sbc_storage_count) >= sbc_frame_size
                   && (s_sbc_storage_count / sbc_frame_size)
                          < SBC_MAX_FRAMES_PER_PACKET))) {
        int16_t pcm[256 * 2];
        pull_pcm(pcm, num_audio_samples);
        s_sbc_encoder->encode_signed_16(&s_sbc_state, pcm,
                                         &s_sbc_storage[1 + s_sbc_storage_count]);
        sbc_frame_size = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
        s_sbc_storage_count += sbc_frame_size;
        s_samples_ready     -= num_audio_samples;
    }
}

/* Common post-send bookkeeping: stretch detector + log non-zero rc.
 * Called by both codecs' send paths so the link-log signal is consistent
 * regardless of which codec is active. */
static void post_send(uint8_t rc, unsigned bytes)
{
    s_packets_sent++;
    if(rc != 0) bt_link_logf("send rc=%u b=%u", rc, bytes);
    uint32_t now = btstack_run_loop_get_time_ms();
    if(s_last_send_ms != 0) {
        uint32_t dt = now - s_last_send_ms;
        /* Send-stretch detector: see comment on s_last_send_ms. 50 ms = ~5
         * missed canonical sends, well outside normal jitter and safely
         * below the audio underrun horizon. */
        if(dt > 50) bt_link_logf("send gap %lu ms", (unsigned long)dt);
    }
    s_last_send_ms = now;
}

static void send_sbc_packet(void)
{
    uint16_t sbc_frame_size = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
    if (sbc_frame_size == 0) { s_sbc_ready_to_send = 0; return; }  /* nothing encoded yet */
    uint8_t num_frames = s_sbc_storage_count / sbc_frame_size;
    if (!s_sbc_logged) {
        s_sbc_logged = true;
        bt_link_logf("SBC bp=%u fsz=%u nf=%u mp=%d",
                     s_sbc_cfg.max_bitpool, sbc_frame_size, num_frames, s_max_payload);
    }
    s_sbc_storage[0] = num_frames;   /* SBC media payload header */
    unsigned bytes = s_sbc_storage_count + 1;
    uint8_t rc = a2dp_source_stream_send_media_payload_rtp(
        s_a2dp_cid, s_local_seid, 0, s_rtp_ts,
        s_sbc_storage, bytes);
    unsigned samples_per_frame = s_sbc_encoder->num_audio_frames(&s_sbc_state);
    s_rtp_ts            += num_frames * samples_per_frame;
    s_sbc_storage_count  = 0;
    s_sbc_ready_to_send  = 0;
    post_send(rc, bytes);
}

/* AAC: 1 frame per RTP packet, no payload header. RTP timestamp advances
 * by 1024 (the AAC LC frame's fundamental sample count) per packet. */
static void send_aac_packet(void)
{
    if(s_aac_payload_size <= 0) {
        /* Stub backend produces 0 bytes — nothing to send. Clear the gate
         * so audio_tick can keep accumulating samples and re-arm send. */
        s_aac_ready_to_send = 0;
        return;
    }
    if(!s_aac_logged_au) {
        s_aac_logged_au = true;
        bt_link_logf("AAC au %02x %02x %02x %02x n=%d mp=%d",
                     s_aac_payload[0], s_aac_payload[1],
                     s_aac_payload[2], s_aac_payload[3], s_aac_payload_size,
                     s_max_payload);
    }
    /* A frame larger than the sink's media MTU can't be sent — the AVDTP
     * layer refuses it (ERROR_CODE_MEMORY_CAPACITY_EXCEEDED) and there is
     * no fragmentation for LATM. vo-aacenc's "CBR" still spikes well above
     * the average (bit reservoir; ~490 B seen at 128 kbps), so a sink with
     * a small MTU would lose frames with only a generic send-rc line to
     * show for it. Drop it here with a distinct log line, and advance the
     * RTP timestamp so the sink's media clock stays consistent. */
    if(s_aac_payload_size > s_max_payload) {
        bt_link_logf("AAC drop %d > mp %d", s_aac_payload_size, s_max_payload);
        s_rtp_ts            += s_aac_input_samples;
        s_aac_payload_size   = 0;
        s_aac_ready_to_send  = 0;
        return;
    }
    uint8_t rc = a2dp_source_stream_send_media_payload_rtp(
        s_a2dp_cid, s_local_seid, 0, s_rtp_ts,
        s_aac_payload, (uint16_t)s_aac_payload_size);
    s_rtp_ts            += s_aac_input_samples;
    int sent_bytes       = s_aac_payload_size;
    s_aac_payload_size   = 0;
    s_aac_ready_to_send  = 0;
    /* Realtime-rate window: ~86 packets/2s is correct (43 fps); a much lower
     * count means we're delivering under realtime → sink underruns to silence. */
    uint32_t twin = btstack_run_loop_get_time_ms();
    if(s_aac_win_start == 0) s_aac_win_start = twin;
    s_aac_win_packets++;
    s_aac_win_bytes += (uint32_t)sent_bytes;
    s_aac_last_size  = sent_bytes;
    if(twin - s_aac_win_start >= 2000) {
        /* avg AU size: ~370 B = real 128k audio reaching the encoder;
         * ~40 B = the encoder is being fed silence (no PCM from playback). */
        bt_link_logf("AAC %lu pkt last=%d avg=%lu",
                     (unsigned long)s_aac_win_packets, s_aac_last_size,
                     (unsigned long)(s_aac_win_bytes /
                         (s_aac_win_packets ? s_aac_win_packets : 1)));
        s_aac_win_start = twin;
        s_aac_win_packets = 0;
        s_aac_win_bytes = 0;
    }
    post_send(rc, (unsigned)sent_bytes);
}

static void audio_tick(btstack_timer_source_t* t)
{
    (void)t;
    if (!s_streaming) return;
    btstack_run_loop_set_timer(&s_audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_audio_timer);

    /* Sample-budget accumulation is codec-agnostic — based purely on the
     * negotiated sample rate and the wall-clock delta since the last tick.
     * Mirrors the original SBC-only code; just sourced from s_sample_rate
     * rather than s_sbc_cfg.freq so AAC fills with the same logic. */
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t dt  = s_time_sent_ms ? (now - s_time_sent_ms) : AUDIO_TIMEOUT_MS;
    uint32_t n   = (dt * s_sample_rate) / 1000;
    s_acc_missed += (dt * s_sample_rate) % 1000;
    while (s_acc_missed >= 1000) { n++; s_acc_missed -= 1000; }
    s_time_sent_ms   = now;
    s_samples_ready += n;

    if(s_codec == BT_PCM_CODEC_SBC) {
        if (s_sbc_ready_to_send) return;
        fill_sbc();
        uint16_t frame_sz = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
        bool payload_full = (uint32_t)(s_sbc_storage_count + frame_sz)
                                > (uint32_t)s_max_payload;
        bool frames_full  = frame_sz > 0
                && (s_sbc_storage_count / frame_sz) >= SBC_MAX_FRAMES_PER_PACKET;
        if (payload_full || frames_full) {
            s_sbc_ready_to_send = 1;
            a2dp_source_stream_endpoint_request_can_send_now(s_a2dp_cid, s_local_seid);
        }
    } else {
        /* AAC: one frame per packet. Encode as soon as we have a frame's
         * worth of samples and the previous send is done, then request
         * can_send_now and let send_aac_packet ship it.
         *
         * The 4 KB PCM scratch buffer lives at file scope rather than on
         * stack — the BT thread's stack is only 8.5 KB and we don't want
         * audio_tick reentrant-ish use to push it close. */
        static int16_t aac_pcm[1024 * 2];
        if (s_aac_ready_to_send || s_aac_payload_size > 0) return;
        if (!s_aac_enc || s_samples_ready < s_aac_input_samples) return;
        pull_pcm(aac_pcm, s_aac_input_samples);
        s_samples_ready -= s_aac_input_samples;
        int out = bt_aac_encoder_encode(s_aac_enc, aac_pcm, s_aac_input_samples,
                                         s_aac_payload, sizeof(s_aac_payload));
        if(out < 0) {
            bt_link_logf("aac enc err %d", out);
            return;
        }
        if(out == 0) {
            /* Encoder is priming (FAAC and most LC encoders return 0 bytes
             * for the first few frames while their analysis lookahead fills).
             * Samples are consumed; just try again next tick. */
            return;
        }
        s_aac_payload_size  = out;
        s_aac_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(s_a2dp_cid, s_local_seid);
    }
}

/* ---- public API ---- */

void bt_pcm_sink_set_sbc_config(uint16_t freq, uint8_t block_length,
                                 uint8_t subbands, uint8_t alloc, uint8_t chmode,
                                 uint8_t max_bitpool)
{
    s_codec                = BT_PCM_CODEC_SBC;
    s_sample_rate          = freq;
    s_sbc_cfg.freq         = freq;
    s_sbc_cfg.block_length = block_length;
    s_sbc_cfg.subbands     = subbands;
    s_sbc_cfg.max_bitpool  = max_bitpool;
    /* AVDTP spec values are 1-based; bluedroid expects 0-based. */
    s_sbc_cfg.allocation_method = (btstack_sbc_allocation_method_t)(alloc - 1);
    switch (chmode) {
    case AVDTP_CHANNEL_MODE_JOINT_STEREO:
        s_sbc_cfg.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
    case AVDTP_CHANNEL_MODE_STEREO:
        s_sbc_cfg.channel_mode = SBC_CHANNEL_MODE_STEREO; break;
    case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
        s_sbc_cfg.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
    default:
        s_sbc_cfg.channel_mode = SBC_CHANNEL_MODE_MONO; break;
    }
    s_sbc_encoder = btstack_sbc_encoder_bluedroid_init_instance(&s_sbc_state);
    s_sbc_encoder->configure(&s_sbc_state, SBC_MODE_STANDARD,
                              s_sbc_cfg.block_length, s_sbc_cfg.subbands,
                              s_sbc_cfg.allocation_method,
                              s_sbc_cfg.freq, s_sbc_cfg.max_bitpool,
                              s_sbc_cfg.channel_mode);
}

/* (Re)build the AAC encoder from the saved s_aac_cfg. Called on a fresh
 * configuration and on stream re-start after a SUSPEND (stop_streaming
 * frees the encoder, but no new MEDIA_CODEC configuration event fires on
 * the re-START, so the saved config is all we have). */
static void aac_encoder_setup(void)
{
    /* Tear down any previous encoder before allocating a fresh one — the
     * sink can be re-configured if the peer renegotiates. */
    if(s_aac_enc) { bt_aac_encoder_free(s_aac_enc); s_aac_enc = NULL; }
    s_aac_enc = bt_aac_encoder_init(s_aac_cfg.sample_rate, s_aac_cfg.channels,
                                    s_aac_cfg.bit_rate, s_aac_cfg.vbr);
    if(s_aac_enc) {
        s_aac_input_samples = bt_aac_encoder_input_samples(s_aac_enc);
        s_aac_max_output    = bt_aac_encoder_max_output(s_aac_enc);
    } else {
        s_aac_input_samples = 1024;     /* fallback so audio_tick doesn't spin */
        s_aac_max_output    = 0;
    }
}

void bt_pcm_sink_set_aac_config(uint32_t sample_rate, uint8_t channels,
                                 uint32_t bit_rate, bool vbr)
{
    s_codec              = BT_PCM_CODEC_AAC;
    s_sample_rate        = sample_rate;
    s_aac_cfg.sample_rate = sample_rate;
    s_aac_cfg.channels    = channels;
    s_aac_cfg.bit_rate    = bit_rate;
    s_aac_cfg.vbr         = vbr;
    aac_encoder_setup();
}

enum bt_pcm_codec bt_pcm_sink_get_codec(void) { return s_codec; }

void bt_pcm_sink_start_streaming(uint16_t a2dp_cid, uint8_t local_seid)
{
    s_a2dp_cid           = a2dp_cid;
    s_local_seid         = local_seid;
    /* Resume after AVDTP SUSPEND: stop_streaming freed the AAC encoder and
     * no configuration event re-creates it on the sink's re-START, so an
     * AAC stream would resume permanently silent (audio_tick bails on
     * s_aac_enc == NULL every tick). Rebuild it from the saved config.
     * SBC is immune — its encoder state isn't torn down on stop. */
    if(s_codec == BT_PCM_CODEC_AAC && !s_aac_enc && s_aac_cfg.sample_rate != 0)
        aac_encoder_setup();
    int max_payload      = a2dp_max_media_payload_size(a2dp_cid, local_seid);
    s_max_payload        = max_payload < SBC_STORAGE_SIZE
                               ? max_payload : SBC_STORAGE_SIZE;
    s_sbc_logged         = false;   /* re-arm one-shot SBC diagnostic */
    s_sbc_storage_count  = 0;
    s_sbc_ready_to_send  = 0;
    s_aac_payload_size   = 0;
    s_aac_ready_to_send  = 0;
    s_aac_logged_au      = false;
    s_aac_win_start      = 0;
    s_aac_win_packets    = 0;
    s_aac_win_bytes      = 0;
    s_aac_last_size      = 0;
    s_time_sent_ms       = 0;
    s_acc_missed         = 0;
    s_samples_ready      = 0;
    s_rtp_ts             = 0;
    s_packets_sent       = 0;
    s_last_send_ms       = 0;
    s_streaming          = true;
    s_active             = true;

    btstack_run_loop_remove_timer(&s_audio_timer);
    btstack_run_loop_set_timer_handler(&s_audio_timer, audio_tick);
    btstack_run_loop_set_timer(&s_audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_audio_timer);
}

void bt_pcm_sink_stop_streaming(void)
{
    s_streaming = false;
    s_active    = false;
    btstack_run_loop_remove_timer(&s_audio_timer);
    if(s_aac_enc) { bt_aac_encoder_free(s_aac_enc); s_aac_enc = NULL; }
}

bool bt_pcm_sink_is_active(void)
{
    return s_active;
}

void bt_pcm_sink_handle_can_send_now(void)
{
    if(!s_streaming) return;
    if(s_codec == BT_PCM_CODEC_SBC) send_sbc_packet();
    else                            send_aac_packet();
}

uint32_t bt_pcm_sink_packets_sent(void)
{
    return s_packets_sent;
}

/* ---- pcm_sink ops ---- */

static void sink_init(void)
{
    /* Lazy: encoder/timer arm via streaming. We do need the mutex up front
     * since pcm_init() may already call sink_lock during boot. */
    if (!s_pcm_lock_inited) {
        mutex_init(&s_pcm_lock);
        s_pcm_lock_inited = true;
    }
}
static void sink_postinit(void) { }

static void sink_set_freq(uint16_t freq)
{
    /* SBC's negotiated rate is fixed once AVDTP configuration completes.
     * If `freq` differs, audio plays at wrong pitch. We advertise only
     * 44.1 kHz in caps, so Rockbox should resample to match. */
    (void)freq;
}

static void sink_lock(void)
{
    /* Caller is the upper PCM layer (pcm_play_data, pcm_set_current_sink,
     * etc.) wanting to mutate pcm_sw_volume's globals. Hold off pull_pcm. */
    mutex_lock(&s_pcm_lock);
}

static void sink_unlock(void)
{
    mutex_unlock(&s_pcm_lock);
}

static void sink_play(const void* addr, size_t size)
{
    /* Caller already holds the sink lock (pcm_play_data → start_pcm →
     * sink->ops.play). Don't re-acquire — that would deadlock on a
     * non-recursive mutex. The STARTED inside this call writes to
     * pcm_sw_volume.c globals but the same lock guards them. */
    s_buf        = (const pcm_sample_t*)addr;
    s_buf_frames = size / PCM_FRAME_BYTES;
    s_buf_pos    = 0;
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

static void sink_stop(void)
{
    /* Caller already holds the sink lock. */
    s_buf        = NULL;
    s_buf_frames = 0;
    s_buf_pos    = 0;
}

static const unsigned long bt_samprs[] = { 44100 };

struct pcm_sink bt_pcm_sink = {
    .caps = {
        .samprs       = bt_samprs,
        .num_samprs   = 1,
        .default_freq = 44100,
    },
    .ops = {
        .init     = sink_init,
        .postinit = sink_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_play,
        .stop     = sink_stop,
    },
};

#endif /* !BOOTLOADER */
