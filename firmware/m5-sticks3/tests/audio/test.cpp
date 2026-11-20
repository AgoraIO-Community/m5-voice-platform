#include "fake.h"
#include "../../main/board_profile.h"

uint64_t sim_ms = 0;
FakeM5 M5;
bool simulate_stick = false;
VoiceBoardProfile voice_board_profile() {
    VoiceBoardProfile profile;
    profile.kind = simulate_stick ? VoiceBoardKind::StickS3 : VoiceBoardKind::Stopwatch;
    profile.name = "HostAudioTest";
    profile.speaker_magnification = simulate_stick ? 16 : 4;
    profile.default_volume_percent = 100;
    return profile;
}
// Include the worker so its infinite task can run under deterministic time.
#include "../../main/voice_audio.cpp"

namespace {
struct Finished {};
unsigned sent = 0;
bool heard_held_audio = false;
voice_audio_codec_t sim_codec;
unsigned remote_interval_ms = 20;
app_vendor_g722_enc_ctx_t remote_encoder;

void remote_frame() {
    int16_t pcm[320]{};
    const size_t samples = remote_interval_ms * 16;
    const bool voiced = (sim_ms >= 640 && sim_ms < 840) ||
                        (sim_ms >= 1130 && sim_ms < 1270);
    if (voiced) {
        for (int i = 0; i < 320; ++i) { pcm[i] = ((i / 8) % 2) ? 5000 : -5000; }
    }
    if (sim_codec == VOICE_AUDIO_CODEC_PCM16) {
        voice_audio_enqueue(sim_codec, pcm, samples * sizeof(int16_t), 16000);
    } else {
        uint8_t encoded[320];
        const size_t count = sim_codec == VOICE_AUDIO_CODEC_G722
            ? app_vendor_g722_encode(&remote_encoder, pcm, samples, encoded)
            : app_codec_g711a_encode(pcm, samples / 2, encoded, sizeof(encoded));
        // An 8 kHz RTP timestamp is legal for G.722; PCM playback is 16 kHz.
        voice_audio_enqueue(sim_codec, encoded, count, 8000);
    }
}

esp_err_t send_frame(const uint8_t *bytes, size_t count, void *) {
    assert(count == 160);
    assert(M5.Mic.running && !M5.Speaker.running);
    // Every frame sent outside the hold must match an independent encoder
    // fed digital silence, even when the fake microphone has nonzero input.
    static uint32_t previous_gate = UINT32_MAX;
    static app_vendor_g722_enc_ctx_t silence_encoder;
    uint32_t gate = s_mic_gate.load();
    if (gate != previous_gate) {
        app_vendor_g722_encoder_init(&silence_encoder, 64000, APP_VENDOR_G722_DEFAULT);
        previous_gate = gate;
    }
    int16_t silence[kSamples] = {};
    uint8_t expected[kSamples / 2];
    app_vendor_g722_encode(&silence_encoder, silence, kSamples, expected);
    const bool carries_audio = memcmp(expected, bytes, count) != 0;
    if (!(gate & 1U)) assert(!carries_audio);
    else if (carries_audio) heard_held_audio = true;
    ++sent;
    return ESP_OK;
}
} // namespace

void vTaskDelay(TickType_t ticks) {
    for (uint32_t n = 0; n < ticks; ++n) {
        ++sim_ms;
        M5.Mic.tick();
        M5.Speaker.tick();
        assert(!M5.Mic.running || !M5.Speaker.running);
        if (sim_ms % remote_interval_ms == 0) { remote_frame(); }
        if (sim_ms == 5) { assert(M5.Speaker.volume == 255); }
        if (sim_ms == 600) { voice_audio_set_volume_percent(80); }
        if (sim_ms == 620) { assert(M5.Speaker.volume == 204); }
        if (sim_ms == 640) { voice_audio_set_volume_percent(60); }
        if (sim_ms == 660) { assert(M5.Speaker.volume == 153); }
        if (sim_ms == 680) { voice_audio_set_volume_percent(40); }
        if (sim_ms == 700) { assert(M5.Speaker.volume == 102); }
        if (sim_ms == 720) { voice_audio_set_volume_percent(20); }
        if (sim_ms == 740) { assert(M5.Speaker.volume == 51); }
        if (sim_ms == 760) { voice_audio_set_volume_percent(0); }
        if (sim_ms == 780) { assert(M5.Speaker.volume == 0); }
        if (sim_ms == 800) { voice_audio_set_volume_percent(100); }
        if (sim_ms == 820) { assert(M5.Speaker.volume == 255); }
        if (sim_ms == 200 || sim_ms == 1600) voice_audio_set_mic_held(true);
        if (sim_ms == 520 || sim_ms == 1100) voice_audio_set_mic_held(false);
        if (sim_ms == 810) voice_audio_set_mic_held(true);
        if (sim_ms == 850) assert(M5.Mic.running && !M5.Speaker.running);
        if (sim_ms == 400) {
            const auto status = voice_audio_get_status();
            assert(status.mic_rms == 100 && !status.user_speaking);
        }
        if (sim_ms == 500) {
            assert(M5.Speaker.starts == 0);
            assert(M5.Mic.running && sent > 10 && heard_held_audio);
            assert(voice_audio_get_status().mic_rms == 1000);
            assert(voice_audio_get_status().user_speaking);
            assert(voice_audio_get_status().playback_level == 0);
        }
        if (sim_ms == 740) {
            const auto status = voice_audio_get_status();
            assert(status.playing && status.playback_level > 0 && status.mic_level == 0);
            unsigned peak = 0;
            for (const auto &request : M5.Speaker.queue) {
                for (const auto sample : request.copy) peak = std::max(peak, unsigned(std::abs(int(sample))));
            }
            assert(peak > (simulate_stick ? 1000U : 5000U) && peak <= kPlaybackTargetPeak);
        }
        if (sim_ms == 900) {
            // Playback is submitted in 40 ms chunks, so a short phrase spans
            // only a few M5.Unified requests instead of one per 10 ms packet.
            assert(M5.Speaker.starts == 1 && M5.Speaker.nonzero >= 2);
        }
        if (sim_ms == 1230) {
            // A fresh reply after interrupting the first one must reach the speaker.
            assert(M5.Speaker.starts == 2 && voice_audio_get_status().playing);
            assert(!M5.Mic.running && M5.Speaker.nonzero >= 4);
        }
        if (sim_ms == 1620) {
            // 40 ms chunks add a bounded phrase-tail delay, but capture must
            // resume well before the next session transition at 1650 ms.
            assert(!M5.Speaker.running && M5.Mic.running && M5.Mic.starts >= 2);
            assert(voice_audio_get_status().playback_level == 0);
        }
        if (sim_ms == 1650) { voice_audio_set_active(false); }
        if (sim_ms == 1690) { assert(!M5.Speaker.running && !M5.Mic.running); }
        if (sim_ms == 1750) { voice_audio_set_active(true); }
        if (sim_ms == 2100) {
            assert(M5.Speaker.starts == 2 && M5.Mic.running && sent > 35);
        }
        if (sim_ms == 2300) {
            // Speech already present throughout startup must survive warmup.
            const auto status = voice_audio_get_status();
            assert(status.user_speaking && status.mic_rms == 1000);
        }
        if (sim_ms == 2350) { throw Finished{}; }
    }
}

int main(int argc, char **argv) {
    sim_codec = argc > 1 ? static_cast<voice_audio_codec_t>(atoi(argv[1]))
                         : VOICE_AUDIO_CODEC_PCM16;
    if (argc > 2) { remote_interval_ms = atoi(argv[2]); }
    simulate_stick = argc > 3 && strcmp(argv[3], "stick") == 0;
    assert(remote_interval_ms == 10 || remote_interval_ms == 20);
    // The StopWatch limiter must lift quiet speech but never exceed its headroom.
    PlaybackBuffer loud{};
    std::fill(std::begin(loud.pcm), std::end(loud.pcm), int16_t(32767));
    float limiter_gain = 1.0f;
    limit_stopwatch_playback(&loud, kPlaySamples, limiter_gain);
    assert(pcm_peak(loud.pcm, kPlaySamples) <= kPlaybackTargetPeak);
    PlaybackBuffer quiet{};
    std::fill(std::begin(quiet.pcm), std::end(quiet.pcm), int16_t(5000));
    limiter_gain = 1.0f;
    limit_stopwatch_playback(&quiet, kPlaySamples, limiter_gain);
    assert(pcm_peak(quiet.pcm, kPlaySamples) > 5000);
    app_vendor_g722_encoder_init(&remote_encoder, 64000, APP_VENDOR_G722_DEFAULT);
    assert(voice_audio_init(nullptr, nullptr) == ESP_ERR_INVALID_ARG);
    assert(voice_audio_init(send_frame, nullptr) == ESP_OK);
    assert(voice_audio_init(send_frame, nullptr) == ESP_ERR_INVALID_STATE);

    int16_t obsolete_voice[320];
    std::fill(std::begin(obsolete_voice), std::end(obsolete_voice), 1000);
    assert(pcm_ac_rms(obsolete_voice, 320) == 0); // DC is not audio activity.
    for (size_t i = 0; i < 320; ++i) { obsolete_voice[i] = (i & 1) ? 5000 : -5000; }
    assert(!voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, obsolete_voice, 640, 16000));
    voice_audio_set_active(true);
    assert(!voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, nullptr, 640, 16000));
    assert(!voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, obsolete_voice, 639, 16000));
    assert(!voice_audio_enqueue(VOICE_AUDIO_CODEC_PCMA, obsolete_voice, 160, 16000));
    for (size_t i = 0; i < s_packet_count; ++i) {
        assert(voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, obsolete_voice, 640, 16000));
    }
    assert(!voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, obsolete_voice, 640, 16000));
    assert(voice_audio_get_status().dropped_pool_full == 1);
    voice_audio_set_active(false);
    voice_audio_set_active(true);
    try { worker(nullptr); } catch (const Finished &) {}
    assert(voice_audio_get_status().dropped_stale == 0);
    printf("PASS board=%s codec=%d packet_ms=%u tx=%u playback=%u nonzero=%u mic_starts=%u speaker_starts=%u\n",
           simulate_stick ? "StickS3" : "StopWatch", int(sim_codec), remote_interval_ms, sent, M5.Speaker.frames, M5.Speaker.nonzero,
           M5.Mic.starts, M5.Speaker.starts);
}
