#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_CODEC_G722 = 0, /* 16 kHz mono, 64 kbit/s, encoded outside the SDK. */
    VOICE_CODEC_PCMA,     /* 8 kHz mono G.711 A-law. */
    VOICE_CODEC_PCM16,    /* Receive only; rate follows the requested codec. */
} voice_codec_t;

typedef enum {
    VOICE_EVENT_JOINED = 0,
    VOICE_EVENT_REJOINED,
    VOICE_EVENT_RECONNECTING,
    VOICE_EVENT_CONNECTION_LOST,
    VOICE_EVENT_REMOTE_JOINED,
    VOICE_EVENT_REMOTE_OFFLINE,
    VOICE_EVENT_TOKEN_EXPIRED,
    VOICE_EVENT_ERROR,
    VOICE_EVENT_UNSUPPORTED_AUDIO,
} voice_transport_event_t;

/* Callbacks run on SDK tasks. They must return promptly, without blocking,
 * taking lifecycle locks, or calling start/stop. Copy audio before returning;
 * its storage belongs to the SDK. Queue events to the application's task.
 * TOKEN_EXPIRED also covers the SDK's advance expiry notification: stop the
 * current session and start again to obtain fresh credentials. */
typedef void (*voice_transport_event_cb_t)(voice_transport_event_t event, int code, void *ctx);
typedef void (*voice_transport_audio_cb_t)(voice_codec_t codec, const void *data,
                                         size_t size, uint32_t pts_ms, void *ctx);
typedef struct {
    const char *base_url;
    const char *device_key;      /* Optional bearer token, requires HTTPS. */
    const char *agent_id;         /* Library selection used during start. */
    voice_codec_t codec;
    uint32_t area_code;          /* Zero selects the SDK's default global region. */
    uint32_t http_timeout_ms;    /* Zero selects 15000 ms. */
    uint32_t join_timeout_ms;    /* Zero selects 15000 ms. */
    const char *get_config_path; /* NULL selects /get_config. */
    const char *start_agent_path;/* NULL selects /startAgent. */
    const char *stop_agent_path; /* NULL selects /stopAgent. */
    voice_transport_event_cb_t event_cb;
    voice_transport_audio_cb_t audio_cb;
    void *ctx;
} voice_transport_config_t;

/* Call start/stop serially from a single control task with at least 8 KiB of
 * stack, after Wi-Fi and clock setup. Start copies all string configuration.
 * Success means RTC joined and startAgent returned a valid agent_id. The
 * REMOTE_JOINED callback can arrive before start returns.
 * Repeated start while active returns ESP_ERR_INVALID_STATE. Stop releases
 * the session's RTC connection even if HTTP stop fails; it retains a pending
 * agent_id so another stop (or the next start) retries cloud cleanup.
 * The RTSA 1.9.7 engine is initialized once per device boot: its bundled
 * AOSL leaks a DNS worker on repeated init/fini. New sessions fetch fresh
 * credentials and create new connections inside the retained engine.
 * Changing App ID or region requires reboot and returns
 * ESP_ERR_INVALID_STATE. Persistent engine RAM is intentional and bounded. */
esp_err_t voice_transport_start(const voice_transport_config_t *config);
esp_err_t voice_transport_stop(void);
typedef bool (*voice_library_item_cb)(const char *id, const char *name, void *context);
esp_err_t voice_transport_library(const char *base_url, const char *device_key, const char *device_id,
                                  voice_library_item_cb item, void *context);

/* Send may run on the capture task concurrently with stop. Data must already
 * be encoded according to config.codec. RTSA 1.9.7 timestamps outgoing frames
 * itself, so pts_ms is accepted for the caller's timeline but not transmitted. */
esp_err_t voice_transport_send_audio(const void *data, size_t size, uint32_t pts_ms);
bool voice_transport_is_ready(void);

#ifdef __cplusplus
}
#endif
