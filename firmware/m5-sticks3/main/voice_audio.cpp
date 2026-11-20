#include "voice_audio.h"

#include <M5Unified.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "board_profile.h"

extern "C" {
#include "app_codec_g711.h"
#include "app_vendor_g722.h"
}

#ifndef VOICE_AUDIO_MIC_GAIN
#define VOICE_AUDIO_MIC_GAIN 4
#endif
#ifndef VOICE_AUDIO_VAD_RMS
#define VOICE_AUDIO_VAD_RMS 450
#endif
#ifndef VOICE_AUDIO_DEFAULT_VOLUME_PERCENT
#define VOICE_AUDIO_DEFAULT_VOLUME_PERCENT 100
#endif
#ifndef VOICE_AUDIO_REMOTE_RMS
#define VOICE_AUDIO_REMOTE_RMS 48
#endif

namespace {
constexpr char kTag[] = "voice_audio";
constexpr size_t kSamples = 320;
constexpr uint32_t kRate = 16000;
constexpr size_t kPacketBytes = 640;
// Real downlink is 80-byte G.722 / 10 ms. The pool only needs to hold that
// packet; 640 bytes still covers the 20 ms PCM frames used by the host test.
// 32 packets leave room for a network burst on boards that have PSRAM, while
// the internal-RAM fallback still covers a 40 ms chunk and scheduling jitter.
constexpr size_t kPacketCountWithPsram = 32;
constexpr size_t kPacketCountInternal = 12;
// Host-side audio tests model the PSRAM-capable path and use this name when
// filling their fake packet pool.
constexpr size_t kPacketCount = kPacketCountWithPsram;
// One playback buffer holds one 40 ms chunk. Two M5.Unified slots plus the
// 80 ms I2S DMA queue provide the jitter cushion without a 60 ms buffer.
constexpr size_t kPlaySamples = 640;
// Submit speech in 40 ms chunks. Two M5.Unified playback slots then provide
// about 80 ms of producer jitter tolerance, instead of the 20 ms available
// when every 10 ms G.722 packet is queued separately.
constexpr size_t kPlaybackChunkMs = 40;
constexpr uint64_t kPartialChunkFlushMs = 25;
// StopWatch runs the ES8311 at +6 dB and M5.Unified at 4x, which leaves the
// decoded TTS waveform rather quiet. Lift low-level speech, but clamp each
// playback chunk before it reaches M5.Unified so loud peaks cannot crackle.
constexpr unsigned kPlaybackTargetPeak = 28000;
constexpr unsigned kPlaybackNoiseGatePeak = 256;
constexpr float kPlaybackMinGain = 0.75f;
constexpr float kPlaybackMaxGain = 3.2f;
constexpr size_t kCaptureBuffers = 3;
// The speaker release callback is installed. Two alternating buffers are
// enough for playback; a third storage slot absorbs a short release callback
// delay without growing the DMA queue.
constexpr size_t kPlaybackBuffers = 2;
constexpr size_t kPlaybackBufferSlots = 3;
// TTS commonly pauses inside a paragraph. Tearing down the shared ES8311
// speaker path on every 120-180 ms pause caused an audible click/stutter and a
// costly Mic<->Speaker transition. Keep the playout path alive long enough to
// bridge natural sentence pauses, then resume capture.
constexpr uint64_t kPlaybackTailMs = 180;
constexpr uint64_t kPlaybackSilenceMs = 450;
constexpr uint64_t kPacketMaxAgeMs = 500;
constexpr uint64_t kMicWarmupMs = 350;
constexpr uint64_t kSpeechHoldMs = 360;
constexpr size_t kPrerollMs = 40;

struct Packet {
    uint32_t generation;
    uint32_t rate;
    uint64_t arrived_ms;
    voice_audio_codec_t codec;
    size_t size;
    uint8_t data[kPacketBytes];
};

enum BufferState : uint8_t { Free, OwnedByMic, Captured };
std::atomic<uint32_t> s_mic_gate{0}; // Odd epochs publish captured PCM.
struct CaptureBuffer {
    uint32_t gate = 0;
    std::atomic<uint8_t> state{Free};
    uint32_t sequence = 0;
    int16_t pcm[kSamples]{};
};
struct PlaybackBuffer {
    std::atomic<bool> owned_by_speaker{false};
    int16_t pcm[kPlaySamples]{};
};

QueueHandle_t s_free_packets = nullptr;
QueueHandle_t s_ready_packets = nullptr;
Packet *s_packets = nullptr;
size_t s_packet_count = 0;
TaskHandle_t s_task = nullptr;
voice_audio_send_callback_t s_send = nullptr;
void *s_send_ctx = nullptr;
// Low bit is the desired active state; the other bits are the session epoch.
std::atomic<uint32_t> s_generation{0};
std::atomic<bool> s_initialized{false};
// Keep the user-facing value separate from M5Unified's 0..255 master-volume
// scale. The worker applies it at its next cadence so the application task
// never calls into the speaker while the audio worker owns its lifecycle.
std::atomic<uint8_t> s_volume_percent{VOICE_AUDIO_DEFAULT_VOLUME_PERCENT};
// ES8311 capture arming survives codec power-down/up cycles. Repeat it only
// once per boot; doing the 3x30 ms sequence on every half-duplex switch was
// starving the downlink worker for roughly 120 ms.
std::atomic<bool> s_es8311_capture_armed{false};
portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
voice_audio_status_t s_status{};
CaptureBuffer s_capture[kCaptureBuffers];
PlaybackBuffer s_playback[kPlaybackBufferSlots];
int16_t s_preroll[kPlaySamples];
app_vendor_g722_enc_ctx_t s_encoder;
app_vendor_g722_dec_ctx_t s_decoder;

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
uint32_t elapsed_us(int64_t started_us) {
    return static_cast<uint32_t>(esp_timer_get_time() - started_us);
}

bool has_external_ram() {
#if defined(ESP_PLATFORM)
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return true;
#endif
}

uint8_t volume_to_master(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    return static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255U + 50U) / 100U);
}

void error(esp_err_t err) {
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_error = err;
    portEXIT_CRITICAL(&s_status_lock);
}
enum class DropReason { Invalid, PoolFull, Stale, Playback };
void drop(DropReason reason) {
    portENTER_CRITICAL(&s_status_lock);
    ++s_status.dropped_packets;
    switch (reason) {
        case DropReason::Invalid: ++s_status.dropped_invalid; break;
        case DropReason::PoolFull: ++s_status.dropped_pool_full; break;
        case DropReason::Stale: ++s_status.dropped_stale; break;
        case DropReason::Playback: ++s_status.dropped_playback; break;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

void mic_released(void *, void *data, size_t length) {
    if (length != kSamples) { return; }
    for (auto &buffer : s_capture) {
        if (data == buffer.pcm) {
            buffer.state.store(Captured, std::memory_order_release);
            return;
        }
    }
}

void speaker_released(void *, const void *data, uint8_t) {
    for (auto &buffer : s_playback) {
        if (data == buffer.pcm) {
            buffer.owned_by_speaker.store(false, std::memory_order_release);
            return;
        }
    }
}

// Only the worker changes peripheral ownership. end() joins the library task,
// so no late callback can mark a reused buffer ready after these resets.
void stop_mic() {
    const int64_t started_us = esp_timer_get_time();
    M5.Mic.end();
    const uint32_t elapsed = elapsed_us(started_us);
    for (auto &buffer : s_capture) { buffer.state.store(Free); }
    portENTER_CRITICAL(&s_status_lock);
    s_status.recording = false;
    s_status.user_speaking = false;
    s_status.mic_level = 0;
    s_status.mic_rms = 0;
    s_status.max_mic_end_us = std::max(s_status.max_mic_end_us, elapsed);
    portEXIT_CRITICAL(&s_status_lock);
}

void stop_speaker() {
    const int64_t started_us = esp_timer_get_time();
    M5.Speaker.end();
    const uint32_t elapsed = elapsed_us(started_us);
    for (auto &buffer : s_playback) { buffer.owned_by_speaker.store(false); }
    portENTER_CRITICAL(&s_status_lock);
    s_status.playing = false;
    s_status.playback_level = 0;
    s_status.max_speaker_end_us = std::max(s_status.max_speaker_end_us, elapsed);
    portEXIT_CRITICAL(&s_status_lock);
}

void recycle(Packet *packet) {
    if (packet) { xQueueSend(s_free_packets, &packet, 0); }
}

void queue_capture(uint32_t &sequence) {
    for (auto &buffer : s_capture) {
        if (M5.Mic.isRecording() >= 2) { break; }
        if (buffer.state.load(std::memory_order_acquire) != Free) { continue; }
        buffer.sequence = sequence++;
        buffer.gate = s_mic_gate.load();
        buffer.state.store(OwnedByMic, std::memory_order_release);
        if (!M5.Mic.record(buffer.pcm, kSamples, kRate, false)) {
            buffer.state.store(Free, std::memory_order_release);
            error(ESP_FAIL);
            break;
        }
    }
}

CaptureBuffer *oldest_capture() {
    CaptureBuffer *oldest = nullptr;
    for (auto &buffer : s_capture) {
        if (buffer.state.load(std::memory_order_acquire) != Captured) { continue; }
        if (!oldest || static_cast<int32_t>(buffer.sequence - oldest->sequence) < 0) {
            oldest = &buffer;
        }
    }
    return oldest;
}

PlaybackBuffer *free_playback(size_t limit) {
    for (size_t i = 0; i < std::min(limit, kPlaybackBufferSlots); ++i) {
        auto &buffer = s_playback[i];
        if (!buffer.owned_by_speaker.load(std::memory_order_acquire)) { return &buffer; }
    }
    return nullptr;
}

bool queue_speaker_audio(const int16_t *pcm, size_t count, uint32_t rate) {
    const int64_t started_us = esp_timer_get_time();
    const bool queued = M5.Speaker.playRaw(pcm, count, rate, false, 1, 0);
    const uint32_t elapsed = elapsed_us(started_us);
    portENTER_CRITICAL(&s_status_lock);
    ++s_status.play_calls;
    if (!queued) { ++s_status.play_failures; }
    s_status.max_play_raw_us = std::max(s_status.max_play_raw_us, elapsed);
    portEXIT_CRITICAL(&s_status_lock);
    return queued;
}

bool start_speaker(uint64_t now, uint64_t &retry_ms) {
    if (now < retry_ms) { return false; }
    const int64_t started_us = esp_timer_get_time();
    const bool started = M5.Speaker.begin();
    const uint32_t elapsed = elapsed_us(started_us);
    portENTER_CRITICAL(&s_status_lock);
    ++s_status.speaker_starts;
    s_status.max_speaker_begin_us = std::max(s_status.max_speaker_begin_us, elapsed);
    portEXIT_CRITICAL(&s_status_lock);
    if (!started) {
        error(ESP_FAIL);
        retry_ms = now + 1000;
        ESP_LOGW(kTag, "Speaker start failed; retrying");
    }
    return started;
}

size_t decode(const Packet &packet, int16_t *out) {
    switch (packet.codec) {
        case VOICE_AUDIO_CODEC_G722:
            return app_vendor_g722_decode(&s_decoder, packet.data,
                                          static_cast<int>(packet.size), out);
        case VOICE_AUDIO_CODEC_PCMA:
            return app_codec_g711a_decode(packet.data, packet.size, out, kPlaySamples);
        case VOICE_AUDIO_CODEC_PCM16:
            // Explicit little-endian decode permits unaligned SDK packet data.
            for (size_t i = 0; i < packet.size / 2; ++i) {
                out[i] = static_cast<int16_t>(packet.data[i * 2] |
                                              (uint16_t(packet.data[i * 2 + 1]) << 8));
            }
            return packet.size / 2;
    }
    return 0;
}

// RTC can deliver silence continuously. Packet arrival is not speech activity.
// Subtract DC so a codec's idle offset does not permanently close the mic.
unsigned pcm_peak(const int16_t *pcm, size_t count) {
    unsigned peak = 0;
    for (size_t i = 0; i < count; ++i) {
        peak = std::max(peak, unsigned(std::abs(int(pcm[i]))));
    }
    return peak;
}

void limit_stopwatch_playback(PlaybackBuffer *buffer, size_t count, float &gain) {
    const unsigned peak = pcm_peak(buffer->pcm, count);
    if (peak <= kPlaybackNoiseGatePeak) {
        // Do not amplify codec idle noise or sentence-pause hiss.
        if (peak) std::fill(buffer->pcm, buffer->pcm + count, int16_t(0));
        return;
    }
    const float desired = std::clamp(float(kPlaybackTargetPeak) / float(peak),
                                     kPlaybackMinGain, kPlaybackMaxGain);
    // Attack immediately on a loud peak; release gently across chunks to avoid
    // pumping when TTS level changes between syllables.
    if (desired < gain) gain = desired;
    else gain += (desired - gain) * 0.5f;
    gain = std::clamp(gain, kPlaybackMinGain, kPlaybackMaxGain);
    for (size_t i = 0; i < count; ++i) {
        const int32_t scaled = static_cast<int32_t>(std::lround(buffer->pcm[i] * gain));
        buffer->pcm[i] = static_cast<int16_t>(std::clamp<int32_t>(scaled, INT16_MIN, INT16_MAX));
    }
}

double pcm_ac_rms(const int16_t *pcm, size_t count) {
    if (!count) { return 0; }
    int64_t sum = 0;
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += pcm[i];
        energy += int64_t(pcm[i]) * pcm[i];
    }
    const uint64_t dc_energy = static_cast<uint64_t>((sum * sum) / count);
    return std::sqrt(double(energy - dc_energy) / count);
}

void remember_preroll(const int16_t *pcm, size_t count, uint32_t rate,
                      size_t &saved_count, uint32_t &saved_rate) {
    const size_t capacity = std::min(kPlaySamples, size_t(rate * kPrerollMs / 1000));
    if (saved_rate != rate) { saved_count = 0; }
    saved_rate = rate;
    const size_t incoming = std::min(capacity, count);
    const size_t retained = std::min(saved_count, capacity - incoming);
    memmove(s_preroll, s_preroll + saved_count - retained, retained * sizeof(int16_t));
    memcpy(s_preroll + retained, pcm + count - incoming, incoming * sizeof(int16_t));
    saved_count = retained + incoming;
}

bool arm_es8311_adc() {
    // StickS3 and M5Unified's StopWatch support use the same ES8311 at 0x18.
    // Its SYSTEM write before I2S starts can ACK without arming the ADC. The
    // library applies this post-clock sequence for StopWatch, but its StickS3
    // callback currently only runs before clock start. Repeat it here after
    // every half-duplex switch so a prior DAC reset cannot leave silent capture.
    if (s_es8311_capture_armed.load(std::memory_order_acquire)) { return true; }
    bool acknowledged = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        vTaskDelay(std::max(TickType_t(1), pdMS_TO_TICKS(30)));
        acknowledged = M5.In_I2C.writeRegister8(0x18, 0x0D, 0x01, 100000);
    }
    if (!acknowledged) { ESP_LOGW(kTag, "ES8311 post-clock ADC setup was not acknowledged"); }
    else { s_es8311_capture_armed.store(true, std::memory_order_release); }
    return acknowledged;
}

void worker(void *) {
    const VoiceBoardProfile board = voice_board_profile();
    const bool use_stopwatch_limiter = board.kind == VoiceBoardKind::Stopwatch;
    const size_t playback_buffer_limit = kPlaybackBuffers;
    const uint64_t playback_tail_ms = kPlaybackTailMs;
    const uint64_t playback_silence_ms = kPlaybackSilenceMs;
    const uint64_t partial_chunk_flush_ms = kPartialChunkFlushMs;
    s_volume_percent.store(std::min<uint8_t>(board.default_volume_percent, 100),
                           std::memory_order_release);
    stop_speaker();
    stop_mic();
    auto mic = M5.Mic.config();
    mic.sample_rate = kRate;
    mic.magnification = VOICE_AUDIO_MIC_GAIN;
    // S3 uses PLL240M while M5's divider calculation uses PLL_D2=120M:
    // its 16-bit divisor therefore already accounts for both I2S slots.
    // This is 32 kHz at WS and 16 kHz after software averaging, not 8 kHz.
    mic.over_sampling = 2;
    mic.noise_filter_level = 0;
    mic.dma_buf_len = 160;
    mic.dma_buf_count = 4;
    mic.task_priority = 5;
    M5.Mic.config(mic);
    M5.Mic.setBufferReleaseCallback(nullptr, mic_released);
    auto speaker = M5.Speaker.config();
    // Use the board profile instead of forcing one mixer gain onto every codec.
    // StickS3 needs more than its library default of 1, while StopWatch already
    // has +6 dB in the codec.
    // Track decoded peaks for headroom diagnostics; M5Unified saturates any
    // over-range output before writing int16 I2S. Keep the board's DAC gain at
    // its library setting and let master volume attenuate it.
    speaker.magnification = std::max<uint8_t>(board.speaker_magnification, 1);
    speaker.sample_rate = kRate;
    // 160 frames at 16 kHz is 10 ms per DMA descriptor; eight descriptors
    // give the I2S engine 80 ms of hardware-side cushion without extra copies.
    speaker.dma_buf_len = 160;
    speaker.dma_buf_count = 8;
    speaker.task_priority = 5;
    M5.Speaker.config(speaker);
    M5.Speaker.setChannelVolume(0, 255);
    uint8_t applied_volume = 0xFF;
    M5.Speaker.setBufferReleaseCallback(nullptr, speaker_released);
    ESP_LOGI(kTag, "Mic: 32000 Hz hardware / 2x averaging -> 16000 Hz mono, gain=%d; output=16000 Hz",
             VOICE_AUDIO_MIC_GAIN);
    ESP_LOGI(kTag, "Speaker: board=%s rate=%lu stereo=%d magnification=%u channel0=255; master volume=%u%%",
             board.name,
             static_cast<unsigned long>(speaker.sample_rate), speaker.stereo,
             unsigned(speaker.magnification), unsigned(s_volume_percent.load()));

    uint32_t generation = UINT32_MAX;
    uint32_t capture_sequence = 0;
    uint64_t mic_started_ms = 0;
    uint64_t last_mic_frame_ms = 0;
    uint64_t playout_end_ms = 0;
    uint64_t last_activity_ms = 0;
    uint64_t retry_ms = 0;
    uint64_t last_speech_ms = 0;
    uint64_t previous_loop_ms = 0;
    bool first_packet = true;
    unsigned speech_attack = 0;
    float noise_floor = 100.0f;
    bool mic_running = false;
    bool speaker_running = false;
    Packet *pending = nullptr;
    PlaybackBuffer *prepared = nullptr;
    size_t prepared_count = 0;
    size_t prepared_target = 0;
    uint8_t prepared_level = 0;
    uint32_t prepared_rate = 0;
    uint64_t prepared_arrived_ms = 0;
    uint64_t prepared_since_ms = 0;
    bool prepared_ready = false;
    float playback_limiter_gain = 1.0f;
    size_t preroll_count = 0;
    uint32_t preroll_rate = 0;
    bool need_preroll = false;
    uint8_t encoded[kSamples / 2];

    for (;;) {
        const uint32_t desired = s_generation.load(std::memory_order_acquire);
        uint64_t now = now_ms();
        const uint8_t desired_volume = s_volume_percent.load(std::memory_order_acquire);
        if (desired_volume != applied_volume) {
            M5.Speaker.setVolume(volume_to_master(desired_volume));
            applied_volume = desired_volume;
        }
        if (previous_loop_ms && generation == desired && (generation & 1)) {
            const uint32_t gap_ms = uint32_t(now - previous_loop_ms);
            portENTER_CRITICAL(&s_status_lock);
            s_status.max_worker_gap_ms = std::max(s_status.max_worker_gap_ms, gap_ms);
            if (speaker_running) {
                s_status.max_playback_gap_ms = std::max(s_status.max_playback_gap_ms, gap_ms);
            }
            portEXIT_CRITICAL(&s_status_lock);
        }
        previous_loop_ms = now;
        if (generation != desired) {
            stop_mic();
            stop_speaker();
            mic_running = speaker_running = false;
            recycle(pending);
            pending = nullptr;
            if (prepared) prepared->owned_by_speaker.store(false, std::memory_order_release);
            prepared = nullptr;
            prepared_count = preroll_count = 0;
            prepared_target = 0;
            prepared_rate = 0;
            prepared_arrived_ms = prepared_since_ms = 0;
            prepared_ready = false;
            playback_limiter_gain = 1.0f;
            need_preroll = false;
            generation = desired;
            app_vendor_g722_encoder_init(&s_encoder, 64000, APP_VENDOR_G722_DEFAULT);
            app_vendor_g722_decoder_init(&s_decoder, 64000, APP_VENDOR_G722_DEFAULT);
            last_activity_ms = playout_end_ms = last_speech_ms = retry_ms = 0;
            speech_attack = 0;
            noise_floor = 100.0f;
            first_packet = true;
            portENTER_CRITICAL(&s_status_lock);
            s_status.last_user_voice_ms = 0;
            s_status.last_playback_ms = 0;
            s_status.last_error = ESP_OK;
            s_status.max_send_us = s_status.max_worker_gap_ms = 0;
            s_status.max_playback_gap_ms = 0;
            s_status.max_decode_us = 0;
            s_status.max_mic_begin_us = s_status.max_mic_end_us = 0;
            s_status.max_speaker_begin_us = s_status.max_speaker_end_us = 0;
            s_status.max_play_raw_us = s_status.max_chunk_build_ms = 0;
            s_status.queue_high_water = 0;
            portEXIT_CRITICAL(&s_status_lock);
            ESP_LOGI(kTag, "Session %s epoch=%lu", (generation & 1) ? "active" : "stopped",
                     static_cast<unsigned long>(generation >> 1));
        }

        // The codec is half-duplex. A held front button takes priority over
        // buffered downlink so a user can talk during an agent reply.
        const bool talking = (s_mic_gate.load() & 1U) != 0;
        if (talking) {
            if (speaker_running) {
                stop_speaker();
                speaker_running = false;
            }
            if (prepared) prepared->owned_by_speaker.store(false, std::memory_order_release);
            prepared = nullptr;
            prepared_count = preroll_count = 0;
            prepared_ready = need_preroll = false;
            last_activity_ms = playout_end_ms = 0;
        }
        // Drain obsolete epochs without resetting a queue that producers use.
        while (!pending && xQueueReceive(s_ready_packets, &pending, 0) == pdTRUE) {
            if (pending->generation == generation && (generation & 1)) { break; }
            recycle(pending);
            pending = nullptr;
        }
        if (!(generation & 1)) {
            vTaskDelay(std::max(TickType_t(1), pdMS_TO_TICKS(5)));
            continue;
        }

        // Drop only frames that are already older than the age limit, and only
        // when a newer frame is waiting. Discarding a short fresh queue chops
        // the start of each phrase and is heard as a stutter.
        while (pending && uxQueueMessagesWaiting(s_ready_packets) > 0 &&
               (s_generation.load(std::memory_order_acquire) != generation ||
                now_ms() - pending->arrived_ms > kPacketMaxAgeMs)) {
            recycle(pending);
            drop(DropReason::Stale);
            pending = nullptr;
            preroll_count = 0;
            if (xQueueReceive(s_ready_packets, &pending, 0) != pdTRUE) { break; }
            if (pending->generation != generation || !(generation & 1)) {
                recycle(pending);
                pending = nullptr;
                break;
            }
        }

        if (pending && !prepared) {
            prepared = free_playback(playback_buffer_limit);
            if (prepared) {
                // Reserve before decoding: this frame is not offered to the
                // speaker until a whole jitter-absorbing chunk is ready.
                prepared->owned_by_speaker.store(true, std::memory_order_release);
                prepared_count = 0;
                prepared_target = std::min(kPlaySamples,
                    std::max<size_t>(1, (size_t(pending->rate) * kPlaybackChunkMs + 999) / 1000));
                prepared_rate = pending->rate;
                prepared_arrived_ms = prepared_since_ms = now;
                prepared_ready = false;
            }
        }

        if (pending && prepared && !prepared_ready) {
            size_t incoming = 0;
            switch (pending->codec) {
                case VOICE_AUDIO_CODEC_G722: incoming = pending->size * 2; break;
                case VOICE_AUDIO_CODEC_PCMA: incoming = pending->size; break;
                case VOICE_AUDIO_CODEC_PCM16: incoming = pending->size / 2; break;
            }
            // A rate change or a full buffer is a natural chunk boundary. Keep
            // the pending packet for the next iteration.
            if (prepared_count && (pending->rate != prepared_rate ||
                                   incoming > kPlaySamples - prepared_count)) {
                prepared_ready = true;
            } else {
                const int64_t decode_started_us = esp_timer_get_time();
                const int count = static_cast<int>(decode(*pending, prepared->pcm + prepared_count));
                const auto decode_us = static_cast<uint32_t>(esp_timer_get_time() - decode_started_us);
                unsigned peak = 0;
                for (int i = 0; i < count; ++i) {
                    peak = std::max(peak, unsigned(std::abs(int(prepared->pcm[prepared_count + i]))));
                }
                portENTER_CRITICAL(&s_status_lock);
                s_status.decoded_samples += count;
                s_status.decoded_peak = std::max(s_status.decoded_peak, uint16_t(peak));
                s_status.decode_time_us += decode_us;
                s_status.max_decode_us = std::max(s_status.max_decode_us, decode_us);
                portEXIT_CRITICAL(&s_status_lock);
                if (first_packet) {
                    ESP_LOGI(kTag, "Downlink first packet: codec=%d bytes=%u PCM=%u rate=%lu duration=%lu us",
                             int(pending->codec), unsigned(pending->size), unsigned(count),
                             static_cast<unsigned long>(pending->rate),
                             static_cast<unsigned long>(count * 1000000 / pending->rate));
                    first_packet = false;
                }
                now = now_ms();
                const bool stale = s_generation.load(std::memory_order_acquire) != generation ||
                                   now - pending->arrived_ms > kPacketMaxAgeMs;
                const double rms = pcm_ac_rms(prepared->pcm + prepared_count, count);
                const bool voiced = !stale && rms > VOICE_AUDIO_REMOTE_RMS;
                const bool keep_gap = !stale && speaker_running &&
                                      now - last_activity_ms < playback_silence_ms;
                if (talking) {
                    // Advance the stream decoder but discard this reply audio.
                    preroll_count = 0;
                } else if (!voiced && !keep_gap) {
                    if (stale) { drop(DropReason::Stale); preroll_count = 0; }
                    else { remember_preroll(prepared->pcm + prepared_count, count, pending->rate,
                                            preroll_count, preroll_rate); }
                } else {
                    if (voiced) last_activity_ms = now;
                    if (prepared_count == 0) prepared_since_ms = now;
                    prepared_count += count;
                    prepared_level = static_cast<uint8_t>(std::min(100U, unsigned(rms) / 35));
                    prepared_arrived_ms = now;
                }
                recycle(pending);
                pending = nullptr;
                if (prepared_count) {
                    const bool queue_empty = uxQueueMessagesWaiting(s_ready_packets) == 0;
                    if (prepared_count >= prepared_target ||
                        (queue_empty && now - prepared_since_ms >= partial_chunk_flush_ms)) {
                        prepared_ready = true;
                    }
                } else {
                    prepared->owned_by_speaker.store(false, std::memory_order_release);
                    prepared = nullptr;
                    prepared_target = prepared_rate = 0;
                    prepared_arrived_ms = prepared_since_ms = 0;
                }
            }
        }

        // If the queue goes quiet after a partial chunk, submit it after the
        // flush interval instead of waiting for another packet that may never
        // arrive. Keep the packet-processing branch above as the fast path.
        if (prepared && !prepared_ready && prepared_count && !pending &&
            uxQueueMessagesWaiting(s_ready_packets) == 0 &&
            now_ms() - prepared_since_ms >= partial_chunk_flush_ms) {
            prepared_ready = true;
        }

        if (prepared && prepared_ready) {
            bool stale = s_generation.load(std::memory_order_acquire) != generation ||
                         now - prepared_arrived_ms > kPacketMaxAgeMs;
            if (!stale) {
                if (mic_running) {
                    stop_mic();
                    mic_running = false;
                    speech_attack = 0;
                }
                if (!speaker_running) {
                    speaker_running = start_speaker(now, retry_ms);
                    need_preroll = speaker_running && preroll_count && preroll_rate == prepared_rate;
                }
                now = now_ms();
                stale = s_generation.load(std::memory_order_acquire) != generation ||
                        now - prepared_arrived_ms > kPacketMaxAgeMs;
            }
            if (stale) {
                prepared->owned_by_speaker.store(false, std::memory_order_release);
                prepared = nullptr;
                prepared_count = prepared_target = 0;
                prepared_rate = 0;
                prepared_arrived_ms = prepared_since_ms = 0;
                prepared_ready = false;
                preroll_count = 0;
                need_preroll = false;
                drop(DropReason::Stale);
            } else if (speaker_running) {
                // The first voiced chunk stays reserved while the preceding
                // preroll is submitted. No second decode, no missing syllable.
                if (need_preroll && M5.Speaker.isPlaying(0) < 2) {
                    auto *prefix = free_playback(playback_buffer_limit);
                    if (prefix) {
                        prefix->owned_by_speaker.store(true, std::memory_order_release);
                        memcpy(prefix->pcm, s_preroll, preroll_count * sizeof(int16_t));
                        if (queue_speaker_audio(prefix->pcm, preroll_count, preroll_rate)) {
                            playout_end_ms = std::max(now, playout_end_ms) +
                                             (preroll_count * 1000 + preroll_rate - 1) / preroll_rate;
                        } else { prefix->owned_by_speaker.store(false, std::memory_order_release); }
                        need_preroll = false;
                    }
                }
                if (!need_preroll && M5.Speaker.isPlaying(0) < 2) {
                    if (use_stopwatch_limiter) {
                        limit_stopwatch_playback(prepared, prepared_count, playback_limiter_gain);
                    }
                    if (queue_speaker_audio(prepared->pcm, prepared_count, prepared_rate)) {
                        playout_end_ms = std::max(now, playout_end_ms) +
                                         (prepared_count * 1000 + prepared_rate - 1) / prepared_rate;
                        portENTER_CRITICAL(&s_status_lock);
                        s_status.playback_level = prepared_level;
                        s_status.played_samples += prepared_count;
                        const uint32_t build_ms = static_cast<uint32_t>(now - prepared_since_ms);
                        s_status.max_chunk_build_ms = std::max(s_status.max_chunk_build_ms, build_ms);
                        portEXIT_CRITICAL(&s_status_lock);
                    } else {
                        prepared->owned_by_speaker.store(false, std::memory_order_release);
                        drop(DropReason::Playback);
                        error(ESP_FAIL);
                    }
                    // On success the release callback owns the handoff back to
                    // the free pool; do not make this buffer reusable early.
                    prepared = nullptr;
                    prepared_count = prepared_target = 0;
                    prepared_rate = 0;
                    prepared_arrived_ms = prepared_since_ms = 0;
                    prepared_ready = false;
                    preroll_count = 0;
                }
            }
        }

        now = now_ms();
        if (speaker_running) {
            const bool queued = M5.Speaker.isPlaying();
            const bool audible = queued || now < playout_end_ms + 40;
            portENTER_CRITICAL(&s_status_lock);
            s_status.playing = audible;
            if (audible) { s_status.last_playback_ms = now; }
            portEXIT_CRITICAL(&s_status_lock);
            if (!prepared && !queued &&
                now >= playout_end_ms + playback_tail_ms &&
                now >= last_activity_ms + playback_tail_ms) {
                stop_speaker();
                speaker_running = false;
            }
        }

        if (!speaker_running && !pending && now >= retry_ms &&
            s_generation.load(std::memory_order_acquire) == generation) {
            if (!mic_running) {
                const int64_t started_us = esp_timer_get_time();
                mic_running = M5.Mic.begin();
                const uint32_t elapsed = elapsed_us(started_us);
                portENTER_CRITICAL(&s_status_lock);
                ++s_status.mic_starts;
                s_status.max_mic_begin_us = std::max(s_status.max_mic_begin_us, elapsed);
                portEXIT_CRITICAL(&s_status_lock);
                bool needs_post_clock_adc_arm = false;
#if defined(ESP_PLATFORM)
                needs_post_clock_adc_arm = M5.getBoard() == m5::board_t::board_M5StickS3;
#endif
                if (mic_running && needs_post_clock_adc_arm && !arm_es8311_adc()) {
                    stop_mic();
                    mic_running = false;
                }
                if (mic_running) {
                    mic_started_ms = last_mic_frame_ms = now_ms();
                    queue_capture(capture_sequence);
                    portENTER_CRITICAL(&s_status_lock);
                    s_status.recording = true;
                    portEXIT_CRITICAL(&s_status_lock);
                } else {
                    error(ESP_FAIL);
                    retry_ms = now + 1000;
                    ESP_LOGW(kTag, "Microphone start failed; retrying");
                }
            }
            if (mic_running) {
                queue_capture(capture_sequence);
                while (auto *buffer = oldest_capture()) {
                    now = now_ms();
                    last_mic_frame_ms = now;
                    // ES8311 restarts can shift the baseline. VAD measures AC
                    // energy; the original PCM still goes to the encoder.
                    const auto rms = static_cast<uint16_t>(pcm_ac_rms(buffer->pcm, kSamples));
                    const bool warming_up = now < mic_started_ms + kMicWarmupMs;
                    const bool speech_hangover = last_speech_ms && now - last_speech_ms < kSpeechHoldMs;
                    // Learn quiet frames during warmup, then track slowly.
                    // Never promote above-gate speech or its hangover into the
                    // ambient estimate, including speech present at startup.
                    if (rms < VOICE_AUDIO_VAD_RMS && !speech_hangover) {
                        const float target = std::min(float(rms), float(VOICE_AUDIO_VAD_RMS) * 0.5f);
                        noise_floor += (target - noise_floor) * (warming_up ? 0.2f : 0.01f);
                    }
                    const float threshold = std::max(float(VOICE_AUDIO_VAD_RMS), noise_floor * 2.8f);
                    const bool publish = (buffer->gate & 1U) && buffer->gate == s_mic_gate.load();
                    const bool voiced = publish && !warming_up && rms > threshold;
                    speech_attack = voiced ? std::min(speech_attack + 1, 3U) : 0;
                    if (speech_attack >= 3) { last_speech_ms = now; }
                    portENTER_CRITICAL(&s_status_lock);
                    ++s_status.captured_frames;
                    s_status.mic_rms = rms;
                    s_status.mic_level = static_cast<uint8_t>(std::min(100U, unsigned(rms) / 35));
                    s_status.user_speaking = last_speech_ms && now - last_speech_ms < kSpeechHoldMs;
                    if (speech_attack >= 3) { s_status.last_user_voice_ms = now; }
                    portEXIT_CRITICAL(&s_status_lock);

                    if (s_generation.load(std::memory_order_acquire) == generation) {
                        // Silent PCM keeps remote VAD endpointing after release.
                        static const int16_t silence[kSamples] = {};
                        static uint32_t encoder_gate = 0;
                        const uint32_t gate = s_mic_gate.load();
                        if (encoder_gate != gate) {
                            app_vendor_g722_encoder_init(&s_encoder, 64000, APP_VENDOR_G722_DEFAULT);
                            encoder_gate = gate;
                        }
                        const int count = app_vendor_g722_encode(&s_encoder,
                            publish && gate == buffer->gate ? buffer->pcm : silence, kSamples, encoded);
                        const int64_t send_started_us = esp_timer_get_time();
                        const esp_err_t result = gate == s_mic_gate.load()
                            ? s_send(encoded, static_cast<size_t>(count), s_send_ctx) : ESP_OK;
                        const auto send_us = static_cast<uint32_t>(esp_timer_get_time() - send_started_us);
                        portENTER_CRITICAL(&s_status_lock);
                        s_status.send_time_us += send_us;
                        s_status.max_send_us = std::max(s_status.max_send_us, send_us);
                        if (result == ESP_OK) { ++s_status.sent_frames; }
                        else { s_status.last_error = result; }
                        portEXIT_CRITICAL(&s_status_lock);
                    }
                    buffer->state.store(Free, std::memory_order_release);
                    queue_capture(capture_sequence);
                    // A new generation or downlink gets priority over upload.
                    if (s_generation.load(std::memory_order_acquire) != generation ||
                        uxQueueMessagesWaiting(s_ready_packets)) { break; }
                }
                now = now_ms();
                if (now - last_mic_frame_ms > 1500) {
                    stop_mic();
                    mic_running = false;
                    retry_ms = now + 500;
                    error(ESP_ERR_TIMEOUT);
                    ESP_LOGW(kTag, "Microphone stalled; restarting capture");
                }
            }
        }
        // This project's tick is 1 ms. A deep queue means the worker fell
        // behind a 10 ms frame stream; yield so it can catch up without
        // waiting out another tick per frame. A short queue keeps the 1 ms
        // pace so the speaker task still gets regular time.
        if (uxQueueMessagesWaiting(s_ready_packets) > 8) {
            taskYIELD();
        } else {
            const bool backlog = pending || uxQueueMessagesWaiting(s_ready_packets);
            vTaskDelay(backlog ? TickType_t(1) : std::max(TickType_t(1), pdMS_TO_TICKS(5)));
        }
    }
}
} // namespace

esp_err_t voice_audio_init(voice_audio_send_callback_t send, void *ctx) {
    if (!send) { return ESP_ERR_INVALID_ARG; }
    if (s_initialized.load(std::memory_order_acquire)) { return ESP_ERR_INVALID_STATE; }
    s_packet_count = has_external_ram()
        ? kPacketCountWithPsram : kPacketCountInternal;
    ESP_LOGI(kTag, "Audio packet pool: %u packets (%s)", unsigned(s_packet_count),
             has_external_ram() ? "PSRAM" : "internal RAM");
    s_packets = static_cast<Packet *>(heap_caps_malloc(sizeof(Packet) * s_packet_count,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_packets) {
        s_packets = static_cast<Packet *>(heap_caps_malloc(sizeof(Packet) * s_packet_count,
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    s_free_packets = xQueueCreate(s_packet_count, sizeof(Packet *));
    s_ready_packets = xQueueCreate(s_packet_count, sizeof(Packet *));
    if (!s_packets || !s_free_packets || !s_ready_packets) {
        if (s_free_packets) { vQueueDelete(s_free_packets); }
        if (s_ready_packets) { vQueueDelete(s_ready_packets); }
        heap_caps_free(s_packets);
        s_free_packets = s_ready_packets = nullptr;
        s_packets = nullptr;
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_packet_count; ++i) {
        Packet *packet = &s_packets[i];
        xQueueSend(s_free_packets, &packet, 0);
    }
    s_send = send;
    s_send_ctx = ctx;
    if (xTaskCreate(worker, "voice_audio", 5120, nullptr, 4, &s_task) != pdPASS) {
        vQueueDelete(s_free_packets);
        vQueueDelete(s_ready_packets);
        heap_caps_free(s_packets);
        s_free_packets = s_ready_packets = nullptr;
        s_packets = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_initialized.store(true, std::memory_order_release);
    portENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

void voice_audio_set_mic_held(bool held) {
    uint32_t current = s_mic_gate.load();
    while (bool(current & 1U) != held &&
           !s_mic_gate.compare_exchange_weak(current, current + 1)) {}
}

void voice_audio_set_active(bool active) {
    if (!s_initialized.load(std::memory_order_acquire)) { return; }
    uint32_t previous = s_generation.load(std::memory_order_acquire);
    for (;;) {
        if (bool(previous & 1) == active) { return; }
        const uint32_t next = ((previous + 2) & ~1U) | (active ? 1U : 0U);
        if (s_generation.compare_exchange_weak(previous, next, std::memory_order_acq_rel)) { break; }
    }
}

bool voice_audio_enqueue(voice_audio_codec_t codec, const void *data, size_t size, uint32_t rate) {
    if (!s_initialized.load(std::memory_order_acquire)) { return false; }
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    if (!(generation & 1)) { return false; }
    if (!data || !size || size > kPacketBytes) { drop(DropReason::Invalid); return false; }
    size_t samples;
    switch (codec) {
        case VOICE_AUDIO_CODEC_G722: rate = 16000; samples = size * 2; break;
        case VOICE_AUDIO_CODEC_PCMA:
            if (!rate) { rate = 8000; }
            if (rate != 8000) { drop(DropReason::Invalid); return false; }
            samples = size;
            break;
        case VOICE_AUDIO_CODEC_PCM16:
            if (size & 1) { drop(DropReason::Invalid); return false; }
            samples = size / 2;
            break;
        default: drop(DropReason::Invalid); return false;
    }
    if (rate < 8000 || rate > 48000 || samples > kPlaySamples || samples * 1000 > rate * 60) {
        drop(DropReason::Invalid); return false;
    }
    Packet *packet = nullptr;
    if (xQueueReceive(s_free_packets, &packet, 0) != pdTRUE) { drop(DropReason::PoolFull); return false; }
    packet->generation = generation;
    packet->codec = codec;
    packet->rate = rate;
    packet->size = size;
    packet->arrived_ms = now_ms();
    memcpy(packet->data, data, size);
    if (generation != s_generation.load(std::memory_order_acquire)) {
        recycle(packet);
        drop(DropReason::Stale);
        return false;
    }
    if (xQueueSend(s_ready_packets, &packet, 0) != pdTRUE) {
        recycle(packet);
        drop(DropReason::PoolFull);
        return false;
    }
    const auto queued_packets = static_cast<uint8_t>(uxQueueMessagesWaiting(s_ready_packets));
    portENTER_CRITICAL(&s_status_lock);
    ++s_status.received_packets;
    s_status.received_bytes += size;
    s_status.last_packet_bytes = size;
    s_status.queue_high_water = std::max(s_status.queue_high_water, queued_packets);
    portEXIT_CRITICAL(&s_status_lock);
    return true;
}

voice_audio_status_t voice_audio_get_status(void) {
    portENTER_CRITICAL(&s_status_lock);
    voice_audio_status_t result = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    if (s_ready_packets) {
        result.queued_packets = static_cast<uint8_t>(
            std::min<unsigned>(uxQueueMessagesWaiting(s_ready_packets), UINT8_MAX));
    }
    if (s_free_packets) {
        result.free_packets = static_cast<uint8_t>(
            std::min<unsigned>(uxQueueMessagesWaiting(s_free_packets), UINT8_MAX));
    }
    result.active = bool(s_generation.load(std::memory_order_acquire) & 1);
    result.volume_percent = s_volume_percent.load(std::memory_order_acquire);
    if (!result.active) {
        result.recording = result.playing = result.user_speaking = false;
        result.mic_level = result.playback_level = result.mic_rms = 0;
    }
    return result;
}

void voice_audio_set_volume_percent(uint8_t percent) {
    s_volume_percent.store(std::min<uint8_t>(percent, 100), std::memory_order_release);
}

uint8_t voice_audio_get_volume_percent(void) {
    return s_volume_percent.load(std::memory_order_acquire);
}
