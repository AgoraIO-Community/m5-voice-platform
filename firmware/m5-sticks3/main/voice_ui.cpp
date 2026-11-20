#include "voice_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "lvgl.h"
#include "voice_ui_font.h"

#ifndef VOICE_UI_SIMULATOR
#include <M5Unified.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "voice_ui_pool.h"
#endif

namespace {
constexpr uint32_t kBackground = 0x02070F;
constexpr uint32_t kSurface = 0x112C46;
constexpr uint32_t kEyeBlue = 0x8FE3FF;
constexpr uint32_t kText = 0xF2F7FC;
constexpr uint32_t kMuted = 0x94AEC6;
constexpr uint32_t kBlue = 0x1EAEFF;
constexpr uint32_t kGreen = 0x67E8C3;
constexpr uint32_t kAmber = 0xFFD07E;
constexpr uint32_t kRed = 0xFF8C91;
constexpr uint32_t kVolumeTrack = 0x3E86B8;
// Eye animation only invalidates when an integer pixel changes, so settled
// screens still stop redrawing. StopWatch uses the merged-rectangle path.
constexpr uint32_t kEyeFrameMs = 20;
constexpr uint32_t kBlinkMs = 150;

uint16_t s_width = 135;
uint16_t s_height = 240;
bool s_round = false;
bool s_stopwatch = false;
bool s_initialized = false;
#ifndef VOICE_UI_SIMULATOR
uint32_t s_max_flush_us = 0;
uint32_t s_max_tick_us = 0;
uint32_t s_rendered_frames = 0;
#endif

lv_obj_t *s_state = nullptr;
lv_obj_t *s_detail = nullptr;
lv_obj_t *s_wifi = nullptr;
lv_obj_t *s_battery = nullptr;
lv_obj_t *s_battery_fill = nullptr;
lv_obj_t *s_button = nullptr;
lv_obj_t *s_action = nullptr;
lv_obj_t *s_progress = nullptr;
lv_obj_t *s_countdown = nullptr;
lv_obj_t *s_volume_track = nullptr;
lv_obj_t *s_volume_fill = nullptr;
VoiceUiSnapshot s_snapshot;
uint32_t s_now = 0;
uint32_t s_last_motion = 0;
lv_area_t s_eye_dirty_area{};
char s_detail_text[64] = {};
int s_volume_inner_height = 88;

// Board-specific eye geometry: centers, open size and maximum gaze offset.
struct EyeLayout {
    int left_x, right_x, center_y, width, height, look_x, look_y;
};
EyeLayout s_eye_layout = {44, 91, 90, 34, 46, 12, 10};

// One eye's expression. Scales are relative to the open eye; lids are the
// covered fraction of the eye height (top: flat lid, bottom: arched "smile").
struct EyePose {
    float x = 0, y = 0, w = 1, h = 1, top = 0, bottom = 0;
};

// Cached geometry so LVGL is only touched when a rounded pixel changes.
struct Shape {
    lv_obj_t *object = nullptr;
    int x = -1, y = -1, w = -1, h = -1, r = -1;
    bool shown = true;
};

struct Eye {
    Shape body, top_lid, bottom_lid;
    EyePose pose;
};
Eye s_eyes[2];
uint32_t s_eye_color = kEyeBlue;
uint32_t s_state_since = 0;
VoiceUiState s_eye_state = VoiceUiState::Idle;
uint32_t s_next_blink = 1800;
uint32_t s_blink_start = 0;
bool s_double_blink = false;
uint32_t s_next_glance = 1200;
float s_glance_x = 0, s_glance_y = 0;
float s_level = 0;
uint32_t s_random = 0x2545F491;
int s_battery_fill_max = 6;

lv_color_t color(uint32_t value) { return lv_color_hex(value); }

lv_obj_t *arc(lv_obj_t *parent, int x, int y, int size,
              uint16_t start, uint16_t end, uint32_t track, uint32_t indicator,
              int width, int16_t min_value, int16_t max_value, int16_t value) {
    auto *object = lv_arc_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, size, size);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_range(object, min_value, max_value);
    lv_arc_set_bg_angles(object, start, end);
    lv_arc_set_angles(object, start, end);
    lv_arc_set_value(object, value);
    lv_obj_set_style_arc_width(object, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(object, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(object, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(object, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(object, color(track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(object, color(indicator), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(object, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(object, LV_OPA_TRANSP, LV_PART_KNOB);
    return object;
}

void hidden(lv_obj_t *object, bool value) {
    if (!object) return;
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == value) return;
    if (value) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height,
              uint32_t fill, int radius) {
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color(fill), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return object;
}

lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, int height,
                const char *text, const lv_font_t *font, uint32_t fill,
                lv_text_align_t align) {
    auto *object = lv_label_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color(fill), 0);
    lv_obj_set_style_text_align(object, align, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_label_set_text(object, text);
    return object;
}

struct Presentation {
    const char *title;
    const char *detail;
    const char *action;
    uint32_t accent;
};

constexpr Presentation kIdlePresentation{"Standby", "Press A to talk", "A  Start", kBlue};

Presentation presentation(VoiceUiState state) {
    switch (state) {
        case VoiceUiState::Idle:       return kIdlePresentation;
        case VoiceUiState::Connecting: return {"Connecting", "Joining voice service", "A  Cancel", kBlue};
        case VoiceUiState::Ready:      return {"Speak now", "Ready to listen", "A  End", kGreen};
        case VoiceUiState::Listening:  return {"Listening", "Hearing your voice", "A  End", kGreen};
        case VoiceUiState::Thinking:   return {"Thinking", "Working on a reply", "A  End", kAmber};
        case VoiceUiState::Speaking:   return {"Speaking", "Playing reply", "A  End", kBlue};
        case VoiceUiState::Ended:      return {"Ended", "Start a new chat", "A  Start again", kMuted};
        case VoiceUiState::Error:      return {"Error", "Check network or server", "A  Retry", kRed};
        case VoiceUiState::Offline:    return {"Offline", "Waiting for network", "A  Retry", kAmber};
    }
    return kIdlePresentation;
}

// The bright Agora blue reads better than kBlue as a light source on black.
uint32_t eye_color_for(uint32_t accent) { return accent == kBlue ? kEyeBlue : accent; }

// Deterministic xorshift: blink and glance timing look organic on device and
// stay byte-identical across simulator runs.
uint32_t random_next() {
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return s_random;
}

uint32_t random_between(uint32_t low, uint32_t high) {
    return low + random_next() % (high - low + 1);
}

bool blinks(VoiceUiState state) {
    return state == VoiceUiState::Idle || state == VoiceUiState::Connecting ||
           state == VoiceUiState::Ready || state == VoiceUiState::Listening ||
           state == VoiceUiState::Speaking;
}

bool glances(VoiceUiState state) {
    return state == VoiceUiState::Idle || state == VoiceUiState::Ready ||
           state == VoiceUiState::Listening || state == VoiceUiState::Speaking;
}

// 0 is open, 1 is closed. A missed blink fires as soon as blinking resumes,
// which gives a natural "wake up" blink when leaving a settled state.
float blink_amount() {
    if (!blinks(s_eye_state)) return 0;
    if (s_now >= s_next_blink) {
        s_blink_start = s_now;
        if (s_double_blink) {
            s_double_blink = false;
            s_next_blink = s_now + random_between(2200, 4800);
        } else if (random_next() % 5 == 0) {
            s_double_blink = true;
            s_next_blink = s_now + kBlinkMs + 90;
        } else {
            s_next_blink = s_now + random_between(2200, 4800);
        }
    }
    const uint32_t t = s_now - s_blink_start;
    if (t >= kBlinkMs) return 0;
    const float half = kBlinkMs / 2.0f;
    return 1.0f - std::fabs(static_cast<float>(t) - half) / half;
}

void update_glance() {
    if (!glances(s_eye_state)) {
        s_glance_x = s_glance_y = 0;
        return;
    }
    if (s_now < s_next_glance) return;
    const bool idle = s_eye_state == VoiceUiState::Idle;
    if (random_next() % 3 == 0) {
        s_glance_x = s_glance_y = 0;
    } else {
        // Conversation states keep eye contact; idle looks around the room.
        const float range = idle ? 1.0f : 0.35f;
        s_glance_x = (static_cast<int>(random_next() % 201) - 100) / 100.0f * range;
        s_glance_y = (static_cast<int>(random_next() % 161) - 80) / 100.0f * range;
    }
    s_next_glance = s_now + (idle ? random_between(900, 2600) : random_between(1400, 3200));
}

// side is -1 for the left eye and +1 for the right eye.
EyePose eye_goal(int side) {
    const float t = static_cast<float>(s_now - s_state_since) / 1000.0f;
    const float level = s_level;
    EyePose goal;
    float perspective = 0;
    switch (s_eye_state) {
        case VoiceUiState::Idle:
            goal.x = s_glance_x;
            goal.y = s_glance_y;
            perspective = s_glance_x;
            break;
        case VoiceUiState::Connecting: {
            // Slow left-right scan while searching for the service.
            const float sweep = std::sin(t * 3.4f);
            goal.x = sweep * 0.9f;
            goal.h = 0.9f;
            perspective = sweep;
            break;
        }
        case VoiceUiState::Ready:
            goal.x = s_glance_x;
            goal.y = s_glance_y;
            goal.w = goal.h = 1.08f;
            perspective = s_glance_x;
            break;
        case VoiceUiState::Listening:
            // Attentive: eyes open wider and lift slightly with the voice.
            goal.x = s_glance_x;
            goal.y = s_glance_y - level * 0.2f;
            goal.w = 1.04f + 0.08f * level;
            goal.h = 1.04f + 0.12f * level;
            perspective = s_glance_x;
            break;
        case VoiceUiState::Thinking:
            // Look up and to the side, one eye squinting.
            goal.x = 0.55f + 0.25f * std::sin(t * 1.6f);
            goal.y = -0.75f;
            goal.top = side < 0 ? 0.22f : 0.0f;
            goal.h = side < 0 ? 0.95f : 1.05f;
            perspective = goal.x;
            break;
        case VoiceUiState::Speaking:
            // Smiling eyes that bounce with the playback level.
            goal.x = s_glance_x * 0.5f;
            goal.y = -0.1f - 0.25f * level;
            goal.h = 0.95f - 0.2f * level;
            goal.bottom = 0.18f + 0.22f * level;
            perspective = goal.x;
            break;
        case VoiceUiState::Ended:
            // Content closed arches.
            goal.y = 0.15f;
            goal.h = 0.75f;
            goal.bottom = 0.55f;
            break;
        case VoiceUiState::Error:
            // Tired, lowered lids.
            goal.y = 0.25f;
            goal.w = 0.9f;
            goal.h = 0.75f;
            goal.top = 0.4f;
            break;
        case VoiceUiState::Offline:
            // Asleep: two thin lines.
            goal.y = 0.3f;
            goal.w = 1.05f;
            goal.h = 0.14f;
            break;
    }
    // The eye nearer the gaze direction appears slightly larger.
    const float near = 1.0f + 0.1f * perspective * static_cast<float>(side);
    goal.w *= near;
    goal.h *= near;
    return goal;
}

float approach(float current, float goal, float rate) {
    const float next = current + (goal - current) * rate;
    return std::fabs(goal - next) < 0.004f ? goal : next;
}

bool place(Shape &shape, int x, int y, int w, int h, int r) {
    bool changed = false;
    const bool show = w > 0 && h > 0;
    if (show != shape.shown) {
        hidden(shape.object, !show);
        shape.shown = show;
        changed = true;
    }
    if (!show) return changed;
    if (x != shape.x || y != shape.y) {
        lv_obj_set_pos(shape.object, x, y);
        changed = true;
    }
    if (w != shape.w || h != shape.h) {
        lv_obj_set_size(shape.object, w, h);
        changed = true;
    }
    if (r != shape.r) {
        lv_obj_set_style_radius(shape.object, r, 0);
        changed = true;
    }
    shape.x = x;
    shape.y = y;
    shape.w = w;
    shape.h = h;
    shape.r = r;
    return changed;
}

bool render_eye(Eye &eye, int center_x, float blink) {
    const auto &layout = s_eye_layout;
    const auto &pose = eye.pose;
    const int w = std::max(4, static_cast<int>(layout.width * pose.w + 0.5f));
    const int h = std::max(3, static_cast<int>(layout.height * pose.h * (1.0f - 0.92f * blink) + 0.5f));
    const int cx = center_x + static_cast<int>(std::lround(pose.x * layout.look_x));
    const int cy = layout.center_y + static_cast<int>(std::lround(pose.y * layout.look_y));
    bool changed = place(eye.body, cx - w / 2, cy - h / 2, w, h, std::min(w, h) * 2 / 5);
    // Lids are background-colored children clipped by the eye body.
    const int top = static_cast<int>(pose.top * h + 0.5f);
    changed |= place(eye.top_lid, -1, -1, top > 0 ? w + 2 : 0, top + 1, 0);
    // A circle 1.5x the eye width meets the eye edges almost vertically,
    // carving the classic ^ ^ smile arch from below.
    const int bottom = static_cast<int>(pose.bottom * h + 0.5f);
    const int lid = w + w / 2;
    changed |= place(eye.bottom_lid, (w - lid) / 2, h - bottom, bottom > 0 ? lid : 0, lid, LV_RADIUS_CIRCLE);
    return changed;
}

void animate(uint32_t dt) {
    const float pose_rate = 1.0f - std::exp(-static_cast<float>(dt) / 90.0f);
    const float level_rate = 1.0f - std::exp(-static_cast<float>(dt) / 60.0f);
    const bool audio = s_eye_state == VoiceUiState::Listening || s_eye_state == VoiceUiState::Speaking;
    const float level = audio ? std::min<unsigned>(s_snapshot.audio_level, 100) / 100.0f : 0.0f;
    s_level = approach(s_level, level, level_rate);
    update_glance();
    const float blink = blink_amount();
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        const auto goal = eye_goal(i ? 1 : -1);
        auto &pose = s_eyes[i].pose;
        pose.x = approach(pose.x, goal.x, pose_rate);
        pose.y = approach(pose.y, goal.y, pose_rate);
        pose.w = approach(pose.w, goal.w, pose_rate);
        pose.h = approach(pose.h, goal.h, pose_rate);
        pose.top = approach(pose.top, goal.top, pose_rate);
        pose.bottom = approach(pose.bottom, goal.bottom, pose_rate);
        changed |= render_eye(s_eyes[i], i ? s_eye_layout.right_x : s_eye_layout.left_x, blink);
    }
    // Child position/size changes produce several small invalid areas. Merge
    // them into one fixed eye rectangle so LVGL renders the face once per tick
    // instead of walking two eyes and their lids as separate redraw jobs.
    if (changed) lv_obj_invalidate_area(lv_scr_act(), &s_eye_dirty_area);
}

void create_eyes(lv_obj_t *parent, const EyeLayout &layout) {
    s_eye_layout = layout;
    const int half_w = layout.width * 3 / 5 + 2;
    const int half_h = layout.height * 3 / 5 + 2;
    s_eye_dirty_area.x1 = std::max(0, layout.left_x - half_w - layout.look_x);
    s_eye_dirty_area.y1 = std::max(0, layout.center_y - half_h - layout.look_y);
    s_eye_dirty_area.x2 = std::min<int>(s_width - 1, layout.right_x + half_w + layout.look_x);
    s_eye_dirty_area.y2 = std::min<int>(s_height - 1, layout.center_y + half_h + layout.look_y);
    for (auto &eye : s_eyes) {
        eye.body.object = box(parent, 0, 0, layout.width, layout.height, s_eye_color, 0);
        eye.top_lid.object = box(eye.body.object, 0, 0, 0, 0, kBackground, 0);
        eye.bottom_lid.object = box(eye.body.object, 0, 0, 0, 0, kBackground, 0);
    }
}

void copy_detail(const char *source, const char *fallback) {
    const char *detail = source && *source ? source : fallback;
    size_t n = 0;
    for (const auto *p = detail; *p;) {
        const auto c = static_cast<unsigned char>(*p);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n + length >= sizeof(s_detail_text) || std::strlen(p) < length) break;
        if (c < 32) { s_detail_text[n++] = ' '; ++p; continue; }
        std::memcpy(s_detail_text + n, p, length);
        n += length;
        p += length;
    }
    s_detail_text[n] = 0;
    if (std::strcmp(lv_label_get_text(s_detail), s_detail_text))
        lv_label_set_text(s_detail, s_detail_text);
}

#ifndef VOICE_UI_SIMULATOR
lv_disp_draw_buf_t s_draw_buffer;
lv_disp_drv_t s_display_driver;
lv_color_t *s_pixels = nullptr;
void *s_lvgl_pool = nullptr;
bool s_lvgl_started = false;

void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    const int64_t started_us = esp_timer_get_time();
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    const bool last = lv_disp_flush_is_last(driver);
    M5.Display.startWrite();
    M5.Display.setAddrWindow(area->x1, area->y1, w, h);
    M5.Display.writePixels(reinterpret_cast<lgfx::rgb565_t *>(pixels), w * h);
    M5.Display.endWrite();
    // StopWatch renders through M5GFX's PSRAM framebuffer. Keep every LVGL
    // strip there until the last dirty area is complete, then send one merged
    // panel rectangle instead of many small QSPI transfers interleaved with
    // the AMOLED scan. This reduces visible tearing without changing color depth.
    if (s_stopwatch && last) M5.Display.display();
    lv_disp_flush_ready(driver);
    s_max_flush_us = std::max(s_max_flush_us,
        static_cast<uint32_t>(esp_timer_get_time() - started_us));
}
#endif
} // namespace

#ifndef VOICE_UI_SIMULATOR
extern "C" void *voice_ui_pool_get(size_t size) {
    return size <= LV_MEM_SIZE ? s_lvgl_pool : nullptr;
}
#endif

bool voice_ui_init() {
    VoiceBoardProfile profile;
    profile.kind = VoiceBoardKind::StickS3;
    profile.name = "M5StickS3";
    profile.width = 135;
    profile.height = 240;
    profile.rotation = 0;
    return voice_ui_init(profile);
}

bool voice_ui_init(const VoiceBoardProfile &profile) {
    if (s_initialized) return true;
    s_width = profile.width;
    s_height = profile.height;
    s_round = profile.round_display;
    s_stopwatch = profile.kind == VoiceBoardKind::Stopwatch;
    if (!s_width || !s_height) return false;

#ifndef VOICE_UI_SIMULATOR
    M5.Display.setRotation(profile.rotation);
    if (M5.Display.width() != s_width || M5.Display.height() != s_height) return false;
    // M5GFX enables auto-display for the StopWatch PSRAM framebuffer. LVGL
    // flushes a dirty region in several horizontal strips, so letting M5GFX
    // push each strip immediately produces multiple panel transactions per
    // frame. Flush the accumulated rectangle once from flush() instead.
    if (s_stopwatch) M5.Display.setAutoDisplay(false);
    const size_t lines = s_round ? 12U : 24U;
    const size_t count = static_cast<size_t>(s_width) * lines;
    s_pixels = static_cast<lv_color_t *>(heap_caps_malloc(count * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_pixels) return false;
    if (!s_lvgl_pool) {
        s_lvgl_pool = heap_caps_malloc_prefer(LV_MEM_SIZE, 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_lvgl_pool) {
        heap_caps_free(s_pixels);
        s_pixels = nullptr;
        ESP_LOGE("voice_ui", "Unable to reserve %u-byte LVGL pool", unsigned(LV_MEM_SIZE));
        return false;
    }
    if (!s_lvgl_started) {
        lv_init();
        s_lvgl_started = true;
        ESP_LOGI("voice_ui", "LVGL pool: %u bytes in %s; transfer buffer: %u bytes internal",
                 unsigned(LV_MEM_SIZE), esp_ptr_external_ram(s_lvgl_pool) ? "PSRAM" : "internal RAM",
                 unsigned(count * sizeof(lv_color_t)));
    }
    lv_disp_draw_buf_init(&s_draw_buffer, s_pixels, nullptr, count);
    lv_disp_drv_init(&s_display_driver);
    s_display_driver.hor_res = s_width;
    s_display_driver.ver_res = s_height;
    s_display_driver.flush_cb = flush;
    s_display_driver.draw_buf = &s_draw_buffer;
    if (!lv_disp_drv_register(&s_display_driver)) {
        heap_caps_free(s_pixels);
        s_pixels = nullptr;
        return false;
    }
#endif
    auto *display = lv_disp_get_default();
    if (!display || lv_disp_get_hor_res(display) != s_width || lv_disp_get_ver_res(display) != s_height)
        return false;
    auto *screen = lv_scr_act();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, color(kBackground), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    if (s_round) {
        // 468 canvas, 466 circular OLED. Volume is a short arc on the right,
        // about 2 o'clock to 4 o'clock, filling upward. It stays clear of
        // the top status row and the bottom A-key action.
        s_volume_track = arc(screen, 44, 44, 380, 320, 40, kVolumeTrack, kBlue,
                             14, 0, 100, 100);
        lv_arc_set_mode(s_volume_track, LV_ARC_MODE_REVERSE);
        s_wifi = label(screen, 156, 70, 22, 18, LV_SYMBOL_WIFI, &lv_font_montserrat_14, kMuted, LV_TEXT_ALIGN_CENTER);
        label(screen, 182, 68, 70, 20, "agora", &lv_font_montserrat_14, kText, LV_TEXT_ALIGN_CENTER);
        s_battery = label(screen, 254, 70, 40, 18, "--", &lv_font_montserrat_12, kMuted, LV_TEXT_ALIGN_RIGHT);
        auto *battery_case = box(screen, 298, 73, 16, 10, kBackground, 2);
        lv_obj_set_style_border_width(battery_case, 1, 0);
        lv_obj_set_style_border_color(battery_case, color(kMuted), 0);
        box(screen, 314, 75, 2, 6, kMuted, 0);
        s_battery_fill = box(screen, 300, 75, 10, 6, kGreen, 1);
        s_battery_fill_max = 10;
        create_eyes(screen, {186, 282, 200, 64, 88, 22, 18});
        s_state = label(screen, 124, 324, 220, 22, kIdlePresentation.title, &voice_ui_font_18, kText, LV_TEXT_ALIGN_CENTER);
        s_detail = label(screen, 134, 348, 200, 16, kIdlePresentation.detail, &voice_ui_font_12, kMuted, LV_TEXT_ALIGN_CENTER);
        s_countdown = label(screen, 134, 348, 200, 16, "", &voice_ui_font_12, kMuted, LV_TEXT_ALIGN_CENTER);
        s_progress = lv_bar_create(screen);
        lv_obj_remove_style_all(s_progress);
        lv_obj_set_pos(s_progress, 164, 366);
        lv_obj_set_size(s_progress, 140, 4);
        s_button = box(screen, 156, 382, 156, 32, kSurface, 16);
        s_action = label(s_button, 0, 8, 156, 16, kIdlePresentation.action, &voice_ui_font_12, kText, LV_TEXT_ALIGN_CENTER);
    } else {
        // Keep connectivity and power visible above the animated face.
        s_wifi = label(screen, 8, 9, 71, 17, "", &lv_font_montserrat_12, kMuted, LV_TEXT_ALIGN_LEFT);
        s_battery = label(screen, 78, 9, 36, 16, "--", &lv_font_montserrat_10, kMuted, LV_TEXT_ALIGN_RIGHT);
        auto *battery_case = box(screen, 117, 11, 10, 8, kBackground, 1);
        lv_obj_set_style_border_width(battery_case, 1, 0);
        lv_obj_set_style_border_color(battery_case, color(kMuted), 0);
        box(screen, 127, 13, 2, 4, kMuted, 0);
        s_battery_fill = box(screen, 119, 13, 6, 4, kGreen, 0);
        box(screen, 8, 29, 119, 1, kSurface, 0);
        create_eyes(screen, {44, 89, 96, 34, 52, 10, 10});
        s_state = label(screen, 5, 160, 125, 26, kIdlePresentation.title, &voice_ui_font_18, kText, LV_TEXT_ALIGN_CENTER);
        s_detail = label(screen, 6, 190, 123, 18, kIdlePresentation.detail, &voice_ui_font_12, kMuted, LV_TEXT_ALIGN_CENTER);
        s_countdown = label(screen, 5, 190, 125, 18, "", &voice_ui_font_12, kMuted, LV_TEXT_ALIGN_CENTER);
        s_progress = lv_bar_create(screen);
        lv_obj_remove_style_all(s_progress);
        lv_obj_set_pos(s_progress, 22, 207);
        lv_obj_set_size(s_progress, 91, 3);
        s_volume_track = box(screen, 124, 46, 6, 90, kSurface, 3);
        s_volume_fill = box(s_volume_track, 1, 1, 4, 88, kBlue, 2);
        s_volume_inner_height = 88;
        s_button = box(screen, 8, 215, 119, 22, kSurface, 7);
        s_action = label(s_button, 0, 3, 119, 18, kIdlePresentation.action, &lv_font_montserrat_12, kText, LV_TEXT_ALIGN_CENTER);
    }

    lv_bar_set_range(s_progress, 0, 1000);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, color(kSurface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress, color(kGreen), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, 3, LV_PART_INDICATOR);
    hidden(s_countdown, true);
    hidden(s_progress, true);
    s_initialized = true;
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_SCROLL_CIRCULAR);
    // Until the application publishes its first status, no voice action is available.
    VoiceUiSnapshot boot{};
    boot.voice_configured = false;
    voice_ui_update(boot);
    animate(1000); // Settle the eyes on their first pose without easing in.
    return true;
}

void voice_ui_update(const VoiceUiSnapshot &snapshot) {
    if (!s_initialized) return;
    s_snapshot = snapshot;
    VoiceUiState display_state = snapshot.state;
    auto view = presentation(display_state);
    const char *detail = snapshot.detail;
    if (!snapshot.voice_configured) {
        if (snapshot.state != VoiceUiState::Error) {
            display_state = snapshot.wifi_connected ? VoiceUiState::Idle : VoiceUiState::Connecting;
            view = snapshot.wifi_connected
                ? Presentation{"Wi-Fi only", "Set voice server", "", kGreen}
                : Presentation{"Joining Wi-Fi", "Waiting for network", "", kBlue};
            detail = nullptr;
        }
        view.action = "B  Volume";
    }
    if (snapshot.title) view.title = snapshot.title;
    if (snapshot.action) view.action = snapshot.action;
    if (snapshot.detail) detail = snapshot.detail;
    if (std::strcmp(lv_label_get_text(s_state), view.title)) lv_label_set_text(s_state, view.title);
    if (std::strcmp(lv_label_get_text(s_action), view.action)) lv_label_set_text(s_action, view.action);
    copy_detail(detail, view.detail);

    const auto accent = color(view.accent);
    if (lv_color_to32(lv_obj_get_style_text_color(s_state, 0)) != lv_color_to32(accent)) {
        lv_obj_set_style_text_color(s_state, accent, 0);
    }
    if (display_state != s_eye_state) {
        s_eye_state = display_state;
        s_state_since = s_now;
        s_next_glance = s_now + 400;
    }
    const uint32_t eye_color = eye_color_for(view.accent);
    if (eye_color != s_eye_color) {
        s_eye_color = eye_color;
        for (auto &eye : s_eyes) lv_obj_set_style_bg_color(eye.body.object, color(eye_color), 0);
    }
    const auto wifi_color = color(snapshot.wifi_connected || snapshot.usb_connected ? kGreen : kMuted);
    if (lv_color_to32(lv_obj_get_style_text_color(s_wifi, 0)) != lv_color_to32(wifi_color))
        lv_obj_set_style_text_color(s_wifi, wifi_color, 0);
    const char *wifi_text = snapshot.wifi_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
    if (!s_round) wifi_text = snapshot.wifi_connected ? LV_SYMBOL_WIFI " Linked" : LV_SYMBOL_WIFI " No link";
    if (snapshot.usb_connected) wifi_text = s_round ? "USB" : "USB Ethernet";
    if (std::strcmp(lv_label_get_text(s_wifi), wifi_text)) lv_label_set_text(s_wifi, wifi_text);

    char battery[8];
    if (snapshot.battery_percent < 0) std::snprintf(battery, sizeof(battery), "--");
    else std::snprintf(battery, sizeof(battery), "%s%d%s", snapshot.charging ? "+" : "",
                       std::clamp(snapshot.battery_percent, 0, 100), s_round ? "" : "%");
    if (std::strcmp(lv_label_get_text(s_battery), battery)) lv_label_set_text(s_battery, battery);
    if (s_round) {
        const int volume = std::clamp<int>(snapshot.volume_percent, 0, 100);
        lv_arc_set_value(s_volume_track, volume);
        const auto track = color(volume == 0 ? 0x421E30 : kVolumeTrack);
        const auto indicator = color(volume == 0 ? 0x7A3650 : kBlue);
        lv_obj_set_style_arc_color(s_volume_track, track, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_volume_track, indicator, LV_PART_INDICATOR);
    } else {
        const int volume_height = std::min<int>(snapshot.volume_percent, 100) * s_volume_inner_height / 100;
        hidden(s_volume_fill, volume_height == 0);
        if (volume_height > 0) {
            if (lv_obj_get_height(s_volume_fill) != volume_height) lv_obj_set_height(s_volume_fill, volume_height);
            if (lv_obj_get_y(s_volume_fill) != s_volume_inner_height + 1 - volume_height)
                lv_obj_set_y(s_volume_fill, s_volume_inner_height + 1 - volume_height);
        }
        const auto volume_track_color = color(volume_height == 0 ? 0x421E30 : kSurface);
        if (lv_color_to32(lv_obj_get_style_bg_color(s_volume_track, 0)) != lv_color_to32(volume_track_color))
            lv_obj_set_style_bg_color(s_volume_track, volume_track_color, 0);
    }

    const int fill = snapshot.battery_percent < 0 ? 0
        : std::clamp(snapshot.battery_percent * s_battery_fill_max / 100, 1, s_battery_fill_max);
    hidden(s_battery_fill, fill == 0);
    if (fill > 0) {
        if (lv_obj_get_width(s_battery_fill) != fill) lv_obj_set_width(s_battery_fill, fill);
        const auto battery_color = color(snapshot.battery_percent < 20 ? kAmber : kGreen);
        if (lv_color_to32(lv_obj_get_style_bg_color(s_battery_fill, 0)) != lv_color_to32(battery_color))
            lv_obj_set_style_bg_color(s_battery_fill, battery_color, 0);
    }

    const bool countdown = !snapshot.action && display_state == VoiceUiState::Ready && snapshot.timeout_ms > 0;
    if (countdown) {
        hidden(s_detail, true);
        hidden(s_countdown, false);
        hidden(s_progress, false);
        const auto remaining = std::min(snapshot.remaining_ms, snapshot.timeout_ms);
        char text[48];
        std::snprintf(text, sizeof(text), "Ends in %lus", static_cast<unsigned long>((remaining + 999) / 1000));
        if (std::strcmp(lv_label_get_text(s_countdown), text)) lv_label_set_text(s_countdown, text);
        const int value = static_cast<int>((static_cast<uint64_t>(remaining) * 1000) / snapshot.timeout_ms);
        if (lv_bar_get_value(s_progress) != value) lv_bar_set_value(s_progress, value, LV_ANIM_OFF);
        const auto progress_color = color(remaining <= 3000 ? kAmber : kGreen);
        lv_obj_set_style_bg_color(s_progress, progress_color, LV_PART_INDICATOR);
    } else {
        hidden(s_detail, false);
        hidden(s_countdown, true);
        hidden(s_progress, true);
    }
    const auto button_color = color(snapshot.voice_configured && snapshot.button_pressed ? 0x176C9E : kSurface);
    if (lv_color_to32(lv_obj_get_style_bg_color(s_button, 0)) != lv_color_to32(button_color))
        lv_obj_set_style_bg_color(s_button, button_color, 0);
}

void voice_ui_tick(uint32_t delta_ms) {
    if (!s_initialized) return;
#ifndef VOICE_UI_SIMULATOR
    const int64_t started_us = esp_timer_get_time();
#endif
    s_now += delta_ms;
    lv_tick_inc(delta_ms);
    if (s_now - s_last_motion >= kEyeFrameMs) {
        animate(s_now - s_last_motion);
        s_last_motion = s_now;
    }
    lv_timer_handler();
#ifndef VOICE_UI_SIMULATOR
    s_max_tick_us = std::max(s_max_tick_us,
        static_cast<uint32_t>(esp_timer_get_time() - started_us));
    ++s_rendered_frames;
#endif
}

VoiceUiDiagnostics voice_ui_get_diagnostics() {
    VoiceUiDiagnostics result{};
#ifndef VOICE_UI_SIMULATOR
    result.max_flush_us = s_max_flush_us;
    result.max_tick_us = s_max_tick_us;
    result.rendered_frames = s_rendered_frames;
#endif
    return result;
}
