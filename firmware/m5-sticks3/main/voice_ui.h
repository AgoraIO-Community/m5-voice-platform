#pragma once

#include <cstdint>

#include "board_profile.h"

// All functions belong to the application/UI task. Network and audio callbacks
// publish their state to that task; they must never call LVGL directly.
enum class VoiceUiState : uint8_t {
    Idle, Connecting, Ready, Listening, Thinking, Speaking, Ended, Error, Offline
};

struct VoiceUiSnapshot {
    VoiceUiState state = VoiceUiState::Idle;
    bool wifi_connected = false;
    int battery_percent = -1; // Negative means unavailable; shown as --.
    bool charging = false;
    uint32_t remaining_ms = 0;
    uint32_t timeout_ms = 15000;
    uint8_t audio_level = 0; // 0..100, sampled from real audio.
    uint8_t volume_percent = 100; // 0..100, master playback volume.
    bool button_pressed = false;
    const char *detail = nullptr; // Optional UI text; copied now. Long text ellipsizes.
    const char *title = nullptr;
    const char *action = nullptr;
    bool voice_configured = true; // False when only local controls are available.
    bool usb_connected = false; // USB Ethernet is the selected network.
};

struct VoiceUiDiagnostics {
    uint32_t max_flush_us;
    uint32_t max_tick_us;
    uint32_t rendered_frames;
};

// Call after M5.begin(). The layout is selected from the runtime board
// profile. The no-argument overload keeps the native portrait simulator and
// small unit fixtures source-compatible.
bool voice_ui_init(const VoiceBoardProfile &profile);
bool voice_ui_init();
void voice_ui_update(const VoiceUiSnapshot &snapshot);
void voice_ui_tick(uint32_t delta_ms);
VoiceUiDiagnostics voice_ui_get_diagnostics();
