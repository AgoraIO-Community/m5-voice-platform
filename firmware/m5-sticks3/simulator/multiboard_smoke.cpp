#include "voice_ui.h"
#include "lvgl.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int width = 135;
int height = 240;
bool round_display = false;
std::vector<lv_color_t> framebuffer;
std::vector<lv_color_t> draw_buffer;
lv_disp_draw_buf_t draw;
lv_disp_drv_t driver;
std::filesystem::path output;

void fail(const char *message) { throw std::runtime_error(message); }

void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= width || y < 0 || y >= height) fail("flush outside display");
            framebuffer[static_cast<size_t>(y) * width + x] = *pixels++;
        }
    }
    lv_disp_flush_ready(drv);
}

void check_bounds(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    if (area.x1 < 0 || area.y1 < 0 || area.x2 >= width || area.y2 >= height)
        fail("UI object outside display");
    if (round_display && lv_obj_check_type(object, &lv_label_class)) {
        // Glyph boxes must stay inside the circular OLED, clear of the bezel.
        const long limit = static_cast<long>(width / 2 - 24);
        const int xs[] = {area.x1, area.x2};
        const int ys[] = {area.y1, area.y2};
        for (int x : xs) {
            for (int y : ys) {
                const long dx = static_cast<long>(x) - width / 2;
                const long dy = static_cast<long>(y) - height / 2;
                if (dx * dx + dy * dy > limit * limit) fail("label outside circular safe area");
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        check_bounds(lv_obj_get_child(object, i));
}

void capture(const char *name) {
    lv_refr_now(nullptr);
    check_bounds(lv_scr_act());
    std::filesystem::create_directories(output);
    const auto path = output / (std::string(name) + ".ppm");
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) fail("capture open");
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (const auto pixel : framebuffer) {
        const auto rgb = lv_color_to32(pixel);
        const uint8_t bytes[] = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
        std::fwrite(bytes, sizeof(bytes), 1, file);
    }
    std::fclose(file);
}

VoiceBoardProfile profile_for(const std::string &name) {
    VoiceBoardProfile profile;
    if (name == "stopwatch") {
        profile.kind = VoiceBoardKind::Stopwatch;
        profile.name = "M5StopWatch";
        profile.width = 468;
        profile.height = 468;
        profile.round_display = true;
    } else {
        profile.kind = VoiceBoardKind::StickS3;
        profile.name = "M5StickS3";
    }
    return profile;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const std::string name = argc > 1 ? argv[1] : "sticks3";
        output = argc > 2 ? argv[2] : (std::filesystem::path("captures") / name);
        const auto profile = profile_for(name);
        width = profile.width;
        height = profile.height;
        round_display = profile.round_display;
        framebuffer.resize(static_cast<size_t>(width) * height);
        draw_buffer.resize(static_cast<size_t>(width) * 24);
        lv_init();
        lv_disp_draw_buf_init(&draw, draw_buffer.data(), nullptr, draw_buffer.size());
        lv_disp_drv_init(&driver);
        driver.hor_res = width;
        driver.ver_res = height;
        driver.draw_buf = &draw;
        driver.flush_cb = flush;
        if (!lv_disp_drv_register(&driver) || !voice_ui_init(profile)) fail("display/UI initialization");
        VoiceUiSnapshot snapshot;
        snapshot.wifi_connected = true;
        snapshot.battery_percent = 87;
        const VoiceUiState states[] = {VoiceUiState::Idle, VoiceUiState::Listening,
                                       VoiceUiState::Thinking, VoiceUiState::Speaking,
                                       VoiceUiState::Error};
        for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
            snapshot.state = states[i];
            snapshot.audio_level = states[i] == VoiceUiState::Speaking ? 90 : 0;
            voice_ui_update(snapshot);
            for (int t = 0; t < 60; ++t) voice_ui_tick(10); // Let the eyes settle.
            char filename[32];
            std::snprintf(filename, sizeof(filename), "%02u-%s", static_cast<unsigned>(i), profile.name);
            capture(filename);
        }
        if (round_display) {
            snapshot.state = VoiceUiState::Idle;
            snapshot.button_pressed = true;
            voice_ui_update(snapshot);
            capture("05-pressed");
            snapshot.button_pressed = false;
            snapshot.state = VoiceUiState::Ready;
            snapshot.remaining_ms = 15000;
            snapshot.timeout_ms = 15000;
            voice_ui_update(snapshot);
            capture("06-ready");
            snapshot.state = VoiceUiState::Idle;
            snapshot.remaining_ms = 0;
            snapshot.volume_percent = 40;
            voice_ui_update(snapshot);
            capture("07-volume-40");
            snapshot.volume_percent = 0;
            voice_ui_update(snapshot);
            capture("08-volume-0");
            snapshot.volume_percent = 100;
            snapshot.charging = true;
            snapshot.battery_percent = 100;
            voice_ui_update(snapshot);
            capture("09-charging");
        }
        snapshot.state = VoiceUiState::Idle;
        snapshot.title = "Device ID";
        snapshot.detail = "AABBCCDDEEFF";
        snapshot.action = "B Next";
        voice_ui_update(snapshot);
        capture("device-id");
        snapshot.title = "Agent";
        snapshot.detail = "Workshop guide";
        snapshot.action = "A Open   B Next";
        voice_ui_update(snapshot);
        capture("agent-picker");
        snapshot.state = VoiceUiState::Ready;
        snapshot.title = "Hold A to talk";
        snapshot.detail = "Microphone muted";
        snapshot.action = "Hold A   B Exit";
        voice_ui_update(snapshot);
        capture("push-to-talk");
        std::printf("PASS: %s %dx%d\n", profile.name, width, height);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
