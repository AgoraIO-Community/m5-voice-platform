#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_AUDIO_CODEC_G722,
    VOICE_AUDIO_CODEC_PCMA,
    VOICE_AUDIO_CODEC_PCM16,
} voice_audio_codec_t;

/* Runs in the audio worker, never in the M5 capture callback. The data is valid
 * for this call only: 160 bytes of G.722 / 320 mono samples at 16 kHz / 20 ms.
 * The transport must consume/copy it before returning and must not block. */
typedef esp_err_t (*voice_audio_send_callback_t)(const uint8_t *data, size_t size, void *ctx);

typedef struct {
    bool initialized;
    bool active;
    bool recording;
    bool playing;
    bool user_speaking;
    uint8_t volume_percent; /* 0..100, master playback volume selected by BtnB */
    uint8_t mic_level; /* 0..100, for animation */
    uint8_t playback_level; /* 0..100, decoded PCM RMS with DC removed */
    uint16_t mic_rms;  /* PCM RMS with DC removed, for calibration */
    uint64_t last_user_voice_ms; /* esp_timer monotonic clock; 0 before speech */
    uint64_t last_playback_ms;   /* actual playout including estimated DMA tail */
    uint32_t captured_frames;
    uint32_t sent_frames;
    uint32_t received_packets;
    uint32_t received_bytes; /* admitted payload bytes, same set as received_packets */
    uint32_t decoded_samples;
    uint16_t decoded_peak; /* maximum absolute decoded PCM sample since boot, 0..32768 */
    uint32_t played_samples;
    uint32_t decode_time_us;
    uint32_t max_decode_us;
    uint32_t send_time_us; /* cumulative time inside transport send callback */
    uint32_t max_send_us; /* maxima reset on each session transition */
    uint32_t max_worker_gap_ms;
    uint32_t max_playback_gap_ms; /* worker gap measured while the speaker was active */
    uint32_t max_mic_begin_us;
    uint32_t max_mic_end_us;
    uint32_t max_speaker_begin_us;
    uint32_t max_speaker_end_us;
    uint32_t max_play_raw_us;
    uint32_t max_chunk_build_ms; /* first decoded packet to playRaw submission */
    uint32_t play_calls;
    uint32_t play_failures;
    uint32_t speaker_starts;
    uint32_t mic_starts;
    uint16_t last_packet_bytes;
    uint8_t queue_high_water;
    uint8_t queued_packets; /* current ready-packet depth when status is sampled */
    uint8_t free_packets;   /* current reusable-packet depth when status is sampled */
    uint32_t dropped_packets;
    uint32_t dropped_invalid;
    uint32_t dropped_pool_full;
    uint32_t dropped_stale;
    uint32_t dropped_playback;
    esp_err_t last_error;
} voice_audio_status_t;

/* Call once after M5.begin() has configured internal mic and speaker. This
 * layer exclusively owns M5.Mic and M5.Speaker until reset. */
esp_err_t voice_audio_init(voice_audio_send_callback_t send, void *ctx);

/* Nonblocking session control. Each transition invalidates old queued audio.
 * The worker stops/restarts peripherals; it remains alive between sessions. */
void voice_audio_set_active(bool active);
void voice_audio_set_mic_held(bool held);

/* Set the master playback volume without touching microphone capture. The
 * value is expressed as a user-facing percentage and is applied by the
 * audio worker to M5.Speaker's 0..255 master-volume range. */
void voice_audio_set_volume_percent(uint8_t percent);
uint8_t voice_audio_get_volume_percent(void);

/* Nonblocking SDK callback entry: copies one mono packet into a bounded pool.
 * G.722 always decodes at 16 kHz (an 8 kHz RTP clock is not its PCM rate).
 * PCMA defaults to 8 kHz; PCM16 is signed little-endian, mono, 8..48 kHz.
 * Packets must be <=640 bytes and <=120 ms. Returns false on inactive/full/
 * unsupported input. Transport must reject callbacks from an old RTC session. */
bool voice_audio_enqueue(voice_audio_codec_t codec, const void *data, size_t size,
                         uint32_t sample_rate);

voice_audio_status_t voice_audio_get_status(void);

#ifdef __cplusplus
}
#endif
