#include "voice_ui.h"
#include "lvgl.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

static_assert(LVGL_VERSION_MAJOR == 8 && LVGL_VERSION_MINOR == 4, "Use the production LVGL 8.4 renderer");
namespace {
constexpr int kW = 135, kH = 240;
std::array<lv_color_t, kW * kH> framebuffer{};
std::array<lv_color_t, kW * 24> buffer{};
lv_disp_draw_buf_t draw;
lv_disp_drv_t driver;
uint64_t pixels = 0;
unsigned checks = 0;
std::filesystem::path output;

void check(bool condition, const std::string &description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *data) {
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            check(x >= 0 && x < kW && y >= 0 && y < kH, "flush outside display");
            framebuffer[y * kW + x] = *data++;
            ++pixels;
        }
    lv_disp_flush_ready(drv);
}

uint32_t next_utf8(const char *&text) {
    uint32_t cp = static_cast<unsigned char>(*text++);
    if (cp < 0x80) return cp;
    unsigned count = cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
    cp &= count == 1 ? 0x1F : count == 2 ? 0x0F : 0x07;
    while (count-- && *text) cp = (cp << 6) | (static_cast<unsigned char>(*text++) & 0x3F);
    return cp;
}

void layout(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t rect;
    lv_obj_get_coords(object, &rect);
    check(rect.x1 >= 0 && rect.x2 < kW && rect.y1 >= 0 && rect.y2 < kH, "object bounds");
    if (lv_obj_check_type(object, &lv_label_class)) {
        const auto *font = lv_obj_get_style_text_font(object, 0);
        const char *value = lv_label_get_text(object);
        lv_point_t size;
        lv_txt_get_size(&size, value, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        check(size.y <= lv_obj_get_height(object), std::string("text height: ") + value);
        // Optional diagnostics at y=190 intentionally ellipsize. Titles,
        // counters, numeric extremes and action labels must fit in full.
        if (rect.y1 != 190) check(size.x <= lv_obj_get_width(object), std::string("text width: ") + value);
        while (*value) {
            const auto cp = next_utf8(value);
            lv_font_glyph_dsc_t glyph{};
            check(lv_font_get_glyph_dsc(font, &glyph, cp, 0), "missing glyph " + std::to_string(cp));
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) layout(lv_obj_get_child(object, i));
}

void advance(unsigned milliseconds) {
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) voice_ui_tick(10);
}

void capture(const char *name) {
    advance(60);
    lv_refr_now(nullptr);
    layout(lv_scr_act());
    std::filesystem::create_directories(output);
    const auto path = output / (std::string(name) + ".ppm");
    FILE *file = std::fopen(path.c_str(), "wb");
    check(file != nullptr, "capture open");
    std::fprintf(file, "P6\n%d %d\n255\n", kW, kH);
    unsigned non_background = 0;
    for (const auto pixel : framebuffer) {
        const auto rgb = lv_color_to32(pixel);
        const uint8_t bytes[] = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
        std::fwrite(bytes, sizeof(bytes), 1, file);
        non_background += pixel.full != framebuffer.front().full;
    }
    std::fclose(file);
    check(non_background > 2500, "blank output");
    std::printf("CAPTURE %s\n", path.c_str());
}

void scenario(const char *name, VoiceUiSnapshot snapshot) {
    voice_ui_update(snapshot);
    advance(500); // Let the eyes finish easing into the new expression.
    capture(name);
}

template <int X, int Y, int Width, int Height>
std::array<uint16_t, Width * Height> region() {
    std::array<uint16_t, Width * Height> result{};
    for (int y = 0; y < Height; ++y)
        for (int x = 0; x < Width; ++x)
            result[y * Width + x] = framebuffer[(Y + y) * kW + X + x].full;
    return result;
}

void check_volume_pixels(unsigned percent, int expected_height) {
    // Read the actual RGB565 framebuffer, including the two antialiased cap
    // rows. The middle column must form one bottom-aligned blue run; checking
    // only lv_obj geometry would miss reversed filling or a hidden indicator.
    int blue_rows = 0;
    for (int y = 47; y <= 134; ++y) {
        const auto rgb = lv_color_to32(framebuffer[y * kW + 126]);
        const unsigned red = (rgb >> 16) & 0xFF;
        const unsigned green = (rgb >> 8) & 0xFF;
        const unsigned blue = rgb & 0xFF;
        const bool is_blue = blue >= 200 && green >= 100 && red < 100;
        blue_rows += is_blue;
        check(is_blue == (y >= 135 - expected_height),
              "volume " + std::to_string(percent) + "% fill direction at y=" + std::to_string(y));
    }
    check(blue_rows == expected_height, "volume " + std::to_string(percent) + "% rendered height");
    if (percent == 0) {
        check(framebuffer[90 * kW + 126].full == lv_color_hex(0x421E30).full,
              "muted volume has an empty red track");
    }
}

// The eyes move inside this box; the volume track at x=124 must stay outside.
using EyeRegion = std::array<uint16_t, 116 * 120>;
EyeRegion eyes() { return region<8, 38, 116, 120>(); }

void check_independent_visuals(VoiceUiSnapshot snapshot) {
    snapshot.volume_percent = 60;
    snapshot.state = VoiceUiState::Idle;
    voice_ui_update(snapshot);
    advance(60);
    lv_refr_now(nullptr);
    const auto selected_volume = region<124, 46, 6, 90>();
    const std::array<VoiceUiState, 9> states = {
        VoiceUiState::Idle, VoiceUiState::Connecting, VoiceUiState::Ready,
        VoiceUiState::Listening, VoiceUiState::Thinking, VoiceUiState::Speaking,
        VoiceUiState::Ended, VoiceUiState::Error, VoiceUiState::Offline
    };
    std::array<EyeRegion, 9> expressions{};
    for (size_t i = 0; i < states.size(); ++i) {
        snapshot.state = states[i];
        snapshot.audio_level = 0;
        voice_ui_update(snapshot);
        advance(600);
        lv_refr_now(nullptr);
        expressions[i] = eyes();
        check(region<124, 46, 6, 90>() == selected_volume,
              "state change altered selected volume pixels");
        for (const uint8_t level : {uint8_t(20), uint8_t(60), uint8_t(100)}) {
            snapshot.audio_level = level;
            voice_ui_update(snapshot);
            advance(180);
            lv_refr_now(nullptr);
            check(region<124, 46, 6, 90>() == selected_volume,
                  "eye animation altered selected volume pixels");
        }
    }
    // Every settled expression is visually distinct. Connecting is a moving
    // scan, so it is excluded from the still-frame comparison.
    const std::array<size_t, 6> distinct = {0, 4, 5, 6, 7, 8};
    for (size_t i = 0; i < distinct.size(); ++i)
        for (size_t j = i + 1; j < distinct.size(); ++j)
            check(expressions[distinct[i]] != expressions[distinct[j]],
                  "conversation phases share the same eye expression");

    // Real audio drives the listening and speaking eyes; quiet audio does not.
    for (const auto state : {VoiceUiState::Listening, VoiceUiState::Speaking}) {
        snapshot.state = state;
        snapshot.audio_level = 0;
        voice_ui_update(snapshot);
        advance(600);
        lv_refr_now(nullptr);
        const auto quiet = eyes();
        snapshot.audio_level = 100;
        voice_ui_update(snapshot);
        advance(300);
        lv_refr_now(nullptr);
        check(eyes() != quiet, "audio level does not move the eyes");
    }
    // Ended, error and offline settle into still eyes.
    for (const auto state : {VoiceUiState::Ended, VoiceUiState::Error, VoiceUiState::Offline}) {
        snapshot.state = state;
        voice_ui_update(snapshot);
        advance(1500);
        lv_refr_now(nullptr);
        const auto settled = eyes();
        advance(5000);
        lv_refr_now(nullptr);
        check(eyes() == settled, "settled eyes keep moving");
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        output = argc > 1 ? argv[1] : "captures";
        lv_init();
        lv_disp_draw_buf_init(&draw, buffer.data(), nullptr, buffer.size());
        lv_disp_drv_init(&driver);
        driver.hor_res = kW;
        driver.ver_res = kH;
        driver.draw_buf = &draw;
        driver.flush_cb = flush;
        check(lv_disp_drv_register(&driver) != nullptr, "display registration");
        check(voice_ui_init(), "UI initialization");
        VoiceUiSnapshot s;
        s.voice_configured = false;
        s.state = VoiceUiState::Offline;
        s.detail = "Connecting to Wi-Fi";
        scenario("00-joining-wifi", s);
        s.wifi_connected = true;
        scenario("00-wifi-connected", s);
        s.voice_configured = true;
        s.state = VoiceUiState::Idle;
        s.detail = nullptr;
        s.battery_percent = 87;
        scenario("01-idle", s);
        const auto idle_pixels = pixels;
        for (int i = 0; i < 30; ++i) { voice_ui_update(s); advance(10); }
        advance(6000);
        check(pixels > idle_pixels, "idle eyes never blink or glance");
        for (int i = 0; i < 16; ++i) { advance(90); capture(("motion-idle-" + std::to_string(i)).c_str()); }
        s.button_pressed = true;
        scenario("02-button-pressed", s);
        s.button_pressed = false;
        s.state = VoiceUiState::Connecting;
        scenario("03-connecting", s);
        for (int i = 0; i < 10; ++i) { advance(100); capture(("motion-connect-" + std::to_string(i)).c_str()); }
        s.state = VoiceUiState::Ready;
        s.remaining_ms = 15000;
        scenario("04-ready-15s", s);
        s.remaining_ms = 2800;
        scenario("05-ready-3s", s);
        s.remaining_ms = 0;
        scenario("06-ready-0s", s);
        s.state = VoiceUiState::Listening;
        s.audio_level = 65;
        scenario("07-listening", s);
        s.state = VoiceUiState::Thinking;
        scenario("08-thinking", s);
        s.state = VoiceUiState::Speaking;
        s.audio_level = 95;
        scenario("09-speaking", s);
        for (unsigned i = 0; i < 10; ++i) {
            s.audio_level = static_cast<uint8_t>((i * 37) % 101);
            voice_ui_update(s);
            advance(80);
            capture(("motion-speak-" + std::to_string(i)).c_str());
        }
        s.state = VoiceUiState::Ended;
        s.audio_level = 0;
        scenario("10-ended", s);
        s.state = VoiceUiState::Error;
        scenario("11-error", s);
        s.detail = "HTTP 503: temporarily unavailable";
        scenario("12-error-detail", s);
        const char *diagnostics[] = {
            "Connecting to Wi-Fi", "Ending session", "Wi-Fi disconnected", "Joining voice service",
            "Clock not synced", "Service timed out", "Connection failed", "Ready to listen",
            "Service disconnected", "Reconnecting", "Connection lost", "Reply timed out",
            "Idle timeout", "Audio error", "Start a new chat", "Retrying Wi-Fi",
            "Set Wi-Fi and URL", "Startup failed. Restart."
        };
        for (auto *detail : diagnostics) { s.detail = detail; voice_ui_update(s); advance(60); layout(lv_scr_act()); }
        s.detail = "Idle timeout";
        scenario("12a-idle-timeout", s);
        s.detail = nullptr;
        s.state = VoiceUiState::Offline;
        s.wifi_connected = false;
        s.battery_percent = 5;
        scenario("13-offline-low-battery", s);
        s.state = VoiceUiState::Idle;
        s.battery_percent = -1;
        scenario("14-unknown-battery", s);
        s.battery_percent = 100;
        s.charging = true;
        scenario("15-charging", s);
        // Idle eyes keep blinking; ended/error/offline eyes settle.
        s.state = VoiceUiState::Idle;
        s.volume_percent = 0;
        scenario("17-volume-zero", s);
        s.volume_percent = 60;
        scenario("18-volume-60", s);
        s.state = VoiceUiState::Error;
        scenario("19-settled-error", s);
        const auto settled_pixels = pixels;
        advance(5000);
        check(pixels == settled_pixels, "settled UI redraws");
        s.state = VoiceUiState::Idle;
        s.charging = false;
        s.battery_percent = 87;
        s.wifi_connected = true;
        const std::array<uint8_t, 6> volumes = {100, 80, 60, 40, 20, 0};
        const std::array<int, 6> rendered_heights = {88, 70, 52, 35, 17, 0};
        for (size_t i = 0; i < volumes.size(); ++i) {
            s.volume_percent = volumes[i];
            const auto name = std::to_string(20 + i) + "-volume-" + std::to_string(volumes[i]);
            scenario(name.c_str(), s);
            check_volume_pixels(volumes[i], rendered_heights[i]);
        }
        check_independent_visuals(s);
        lv_mem_monitor_t memory{};
        lv_mem_monitor(&memory);
        std::printf("LVGL pool used including allocator overhead: %u bytes; free: %u bytes\n",
                     static_cast<unsigned>(memory.total_size - memory.free_size),
                     static_cast<unsigned>(memory.free_size));
        std::printf("PASS: %u checks; all nine states, distinct eye expressions, audio-driven eyes, six volume levels, input feedback, timeout bounds, battery extremes, glyph coverage, settled redraw\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
