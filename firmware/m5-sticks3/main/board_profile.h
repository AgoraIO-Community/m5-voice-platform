#pragma once

#include <cstdint>

// The board is identified by M5Unified after M5.begin().  Keep this small
// profile independent from the cloud/session layer so the same voice service
// can run on boards with different screens and input layouts.
enum class VoiceBoardKind : uint8_t {
    StickS3,
    Stopwatch,
    CoreS3,
    Unknown,
};

struct VoiceBoardProfile {
    VoiceBoardKind kind = VoiceBoardKind::Unknown;
    const char *name = "Unknown";
    uint16_t width = 135;
    uint16_t height = 240;
    uint8_t rotation = 0;
    bool round_display = false;
    bool has_secondary_button = true;
    bool needs_post_clock_adc_arm = false;
    // Board-tuned mixer gain. Keep this close to what M5Unified selected for
    // the board; forcing one large value on every codec overdrives the PA and
    // makes the speaker path less tolerant of rail/current transients.
    uint8_t speaker_magnification = 8;
    // Conservative boot volume. The user can still change it with B / arrows.
    uint8_t default_volume_percent = 80;
};

VoiceBoardProfile voice_board_profile();
const char *voice_board_kind_name(VoiceBoardKind kind);
