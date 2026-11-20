#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258;
constexpr int ESP_ERR_INVALID_STATE = 259, ESP_ERR_NO_MEM = 257, ESP_ERR_TIMEOUT = 263;
using TickType_t = uint32_t;
using TaskHandle_t = void *;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) uint32_t(x)
#define pdTRUE 1
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

struct FakeQueue { size_t capacity; std::deque<void *> items; };
using QueueHandle_t = FakeQueue *;
inline QueueHandle_t xQueueCreate(size_t n, size_t item_size) {
    assert(item_size == sizeof(void *));
    return new FakeQueue{n, {}};
}
inline int xQueueSend(QueueHandle_t queue, const void *item, int wait) {
    assert(wait == 0);
    if (queue->items.size() >= queue->capacity) { return 0; }
    queue->items.push_back(*static_cast<void *const *>(item));
    return 1;
}
inline int xQueueReceive(QueueHandle_t queue, void *out, int wait) {
    assert(wait == 0);
    if (queue->items.empty()) { return 0; }
    *static_cast<void **>(out) = queue->items.front();
    queue->items.pop_front();
    return 1;
}
inline void vQueueDelete(QueueHandle_t queue) { delete queue; }
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->items.size(); }
inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
inline void heap_caps_free(void *p) { free(p); }
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, TaskHandle_t *out) {
    *out = reinterpret_cast<void *>(1);
    return 1;
}
extern uint64_t sim_ms;
inline int64_t esp_timer_get_time() { return sim_ms * 1000; }
void vTaskDelay(TickType_t);

struct AudioConfig {
    uint32_t sample_rate = 16000;
    bool stereo = false;
    int magnification = 0, over_sampling = 1, noise_filter_level = 0;
    int dma_buf_len = 0, dma_buf_count = 0, task_priority = 0;
};

struct FakeMic {
    AudioConfig cfg;
    bool running = false;
    unsigned starts = 0;
    void (*callback)(void *, void *, size_t) = nullptr;
    struct Request { int16_t *pcm; size_t count; uint64_t end; };
    std::deque<Request> queue;

    void end() { running = false; queue.clear(); }
    bool begin() { running = true; ++starts; return true; }
    AudioConfig config() { return cfg; }
    void config(AudioConfig value) { cfg = value; }
    void setBufferReleaseCallback(void *, void (*fn)(void *, void *, size_t)) { callback = fn; }
    size_t isRecording() { return queue.size(); }
    bool record(int16_t *pcm, size_t count, uint32_t rate, bool stereo) {
        assert(running && queue.size() < 2 && rate == 16000 && !stereo);
        queue.push_back({pcm, count, (queue.empty() ? sim_ms : queue.back().end) + 20});
        return true;
    }
    void tick() {
        while (!queue.empty() && queue.front().end <= sim_ms) {
            const auto request = queue.front();
            queue.pop_front();
            const int amplitude = sim_ms < 430 ? 100 : 1000;
            for (size_t i = 0; i < request.count; ++i) {
                request.pcm[i] = 1800 + ((i & 1) ? amplitude : -amplitude);
            }
            callback(nullptr, request.pcm, request.count);
        }
    }
};

struct FakeSpeaker {
    AudioConfig cfg;
    FakeSpeaker() { cfg.magnification = 1; cfg.stereo = true; } // StickS3 board defaults.
    bool running = false;
    unsigned volume = 0; // M5Unified master-volume scale (0..255).
    unsigned channel_volume = 255;
    unsigned starts = 0, frames = 0, nonzero = 0;
    void (*callback)(void *, const void *, uint8_t) = nullptr;
    struct Request { const int16_t *pcm; std::vector<int16_t> copy; uint64_t end; };
    std::deque<Request> queue;

    void end() { running = false; queue.clear(); }
    bool begin() { running = true; ++starts; return true; }
    AudioConfig config() { return cfg; }
    void config(AudioConfig value) { cfg = value; }
    void setVolume(int value) { volume = static_cast<unsigned>(value); }
    void setChannelVolume(uint8_t channel, uint8_t value) { assert(channel == 0); channel_volume = value; }
    void setBufferReleaseCallback(void *, void (*fn)(void *, const void *, uint8_t)) { callback = fn; }
    bool isPlaying() { return !queue.empty(); }
    size_t isPlaying(int) { return queue.size(); }
    bool playRaw(const int16_t *pcm, size_t count, uint32_t rate, bool stereo, int, int) {
        assert(running && queue.size() < 2 && !stereo);
        ++frames;
        for (size_t i = 0; i < count; ++i) {
            if (abs(pcm[i]) > 500) { ++nonzero; break; }
        }
        queue.push_back({pcm, {pcm, pcm + count},
                         (queue.empty() ? sim_ms : queue.back().end) + count * 1000 / rate});
        return true;
    }
    void tick() {
        while (!queue.empty() && queue.front().end <= sim_ms) {
            const auto request = queue.front();
            queue.pop_front();
            assert(std::equal(request.copy.begin(), request.copy.end(), request.pcm));
            callback(nullptr, request.pcm, 0);
        }
    }
};

struct FakeI2C { bool writeRegister8(int, int, int, int) { return true; } };
struct FakeM5 { FakeMic Mic; FakeSpeaker Speaker; FakeI2C In_I2C; };
extern FakeM5 M5;
