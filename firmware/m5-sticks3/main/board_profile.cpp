#include "board_profile.h"

#include <M5Unified.h>

const char *voice_board_kind_name(VoiceBoardKind kind) {
    switch (kind) {
        case VoiceBoardKind::StickS3: return "M5StickS3";
        case VoiceBoardKind::Stopwatch: return "M5StopWatch";
        case VoiceBoardKind::CoreS3: return "M5StackCoreS3";
        case VoiceBoardKind::Unknown: break;
    }
    return "Unknown";
}

VoiceBoardProfile voice_board_profile() {
    VoiceBoardProfile profile;
    switch (M5.getBoard()) {
        case m5::board_t::board_M5StackCoreS3:
            profile.kind = VoiceBoardKind::CoreS3;
            profile.name = voice_board_kind_name(profile.kind);
            profile.rotation = M5.Display.getRotation();
            profile.width = M5.Display.width();
            profile.height = M5.Display.height();
            profile.speaker_magnification = 4;
            break;
        case m5::board_t::board_M5StickS3:
            profile.kind = VoiceBoardKind::StickS3;
            profile.name = voice_board_kind_name(profile.kind);
            profile.width = 135;
            profile.height = 240;
            profile.rotation = 0;
            profile.round_display = false;
            profile.has_secondary_button = true;
            profile.needs_post_clock_adc_arm = true;
            // 20 was audibly loud on StickS3 but left little headroom. 16 is
            // still above the library default and was verified with the
            // existing G.722 voice path.
            profile.speaker_magnification = 16;
            profile.default_volume_percent = 100;
            break;
        case m5::board_t::board_M5StopWatch:
            profile.kind = VoiceBoardKind::Stopwatch;
            profile.name = voice_board_kind_name(profile.kind);
            // The controller canvas is 468x468; the OLED's active circular
            // area is 466x466 (the two-pixel edge is outside the safe UI).
            profile.width = 468;
            profile.height = 468;
            profile.rotation = 0;
            profile.round_display = true;
            profile.has_secondary_button = true;
            // M5Unified's StopWatch post-start callback performs the ES8311
            // clock-time ADC arm; do not repeat StickS3's I2C sequence.
            profile.needs_post_clock_adc_arm = false;
            // M5Unified uses 4x here because the StopWatch ES8311 path already
            // adds +6 dB. Higher mixer gain can clip before the master volume
            // is low enough to sound quiet, which is heard as crackle/distortion.
            profile.speaker_magnification = 4;
            profile.default_volume_percent = 100;
            break;
        default:
            profile.name = voice_board_kind_name(profile.kind);
            profile.width = static_cast<uint16_t>(M5.Display.width());
            profile.height = static_cast<uint16_t>(M5.Display.height());
            profile.rotation = 0;
            profile.round_display = false;
            profile.has_secondary_button = true;
            profile.needs_post_clock_adc_arm = false;
            break;
    }
    return profile;
}
