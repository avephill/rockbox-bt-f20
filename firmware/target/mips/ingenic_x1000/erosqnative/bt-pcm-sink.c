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

/* ---- packet assembly ---- */
#define SBC_STORAGE_SIZE 1030
static uint8_t  s_sbc_storage[SBC_STORAGE_SIZE];
static int      s_sbc_storage_count;
static int      s_sbc_ready_to_send;
static int      s_max_payload;

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
    uint16_t sbc_frame_size    = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
    while (s_samples_ready >= num_audio_samples
           && (s_max_payload - s_sbc_storage_count) >= sbc_frame_size) {
        int16_t pcm[256 * 2];
        pull_pcm(pcm, num_audio_samples);
        s_sbc_encoder->encode_signed_16(&s_sbc_state, pcm,
                                         &s_sbc_storage[1 + s_sbc_storage_count]);
        s_sbc_storage_count += sbc_frame_size;
        s_samples_ready     -= num_audio_samples;
    }
}

static void send_media_packet(void)
{
    uint16_t sbc_frame_size = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
    uint8_t num_frames = s_sbc_storage_count / sbc_frame_size;
    s_sbc_storage[0] = num_frames;   /* SBC media payload header */
    uint8_t rc = a2dp_source_stream_send_media_payload_rtp(
        s_a2dp_cid, s_local_seid, 0, s_rtp_ts,
        s_sbc_storage, s_sbc_storage_count + 1);
    unsigned samples_per_frame = s_sbc_encoder->num_audio_frames(&s_sbc_state);
    s_rtp_ts            += num_frames * samples_per_frame;
    s_sbc_storage_count  = 0;
    s_sbc_ready_to_send  = 0;
    s_packets_sent++;

    if(rc != 0) {
        bt_link_logf("send rc=%u nf=%u", rc, num_frames);
    }
    /* Send-stretch detector: if the gap from the previous successful send
     * is well above the canonical 10 ms cadence we are stalled at the
     * controller/RF layer. Threshold 50 ms = ~5 missed sends, well outside
     * normal jitter and safely below the audio buffer's underrun horizon. */
    uint32_t now = btstack_run_loop_get_time_ms();
    if(s_last_send_ms != 0) {
        uint32_t dt = now - s_last_send_ms;
        if(dt > 50) bt_link_logf("send gap %u ms", dt);
    }
    s_last_send_ms = now;
}

static void audio_tick(btstack_timer_source_t* t)
{
    (void)t;
    if (!s_streaming) return;
    btstack_run_loop_set_timer(&s_audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_audio_timer);

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t dt  = s_time_sent_ms ? (now - s_time_sent_ms) : AUDIO_TIMEOUT_MS;
    uint32_t n   = (dt * s_sbc_cfg.freq) / 1000;
    s_acc_missed += (dt * s_sbc_cfg.freq) % 1000;
    while (s_acc_missed >= 1000) { n++; s_acc_missed -= 1000; }
    s_time_sent_ms   = now;
    s_samples_ready += n;

    if (s_sbc_ready_to_send) return;
    fill_sbc();
    uint16_t frame_sz = s_sbc_encoder->sbc_buffer_length(&s_sbc_state);
    if ((uint32_t)(s_sbc_storage_count + frame_sz) > (uint32_t)s_max_payload) {
        s_sbc_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(s_a2dp_cid, s_local_seid);
    }
}

/* ---- public API ---- */

void bt_pcm_sink_set_sbc_config(uint16_t freq, uint8_t block_length,
                                 uint8_t subbands, uint8_t alloc, uint8_t chmode,
                                 uint8_t max_bitpool)
{
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

void bt_pcm_sink_start_streaming(uint16_t a2dp_cid, uint8_t local_seid)
{
    s_a2dp_cid           = a2dp_cid;
    s_local_seid         = local_seid;
    int max_payload      = a2dp_max_media_payload_size(a2dp_cid, local_seid);
    s_max_payload        = max_payload < SBC_STORAGE_SIZE
                               ? max_payload : SBC_STORAGE_SIZE;
    s_sbc_storage_count  = 0;
    s_sbc_ready_to_send  = 0;
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
}

bool bt_pcm_sink_is_active(void)
{
    return s_active;
}

void bt_pcm_sink_handle_can_send_now(void)
{
    /* Just clear the back-pressure block. audio_tick will pick up sending
     * again on the next iteration (and may now send multiple packets). */
    if (s_streaming) send_media_packet();
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
