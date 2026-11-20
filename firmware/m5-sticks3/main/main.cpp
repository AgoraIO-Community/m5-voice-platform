#include <M5Unified.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <string>
#include <vector>
#include <memory>
#include "esp_mac.h"
#include "freertos/queue.h"
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_config.h"
#include "voice_audio.h"
#include "board_profile.h"
#include "voice_heap_diag.h"
#include "voice_transport.h"
#include "voice_ui.h"
#include "voice_usb_net.h"

namespace {
const char *TAG = "m5stack";
VoiceBoardProfile s_board_profile;
std::atomic<bool> wifi_ready{false};
std::atomic<uint32_t> wifi_generation{0};
enum class NetworkLink { None, Wifi, Usb };
std::atomic<NetworkLink> active_link{NetworkLink::None};
bool network_ready() { return active_link.load() != NetworkLink::None; }
std::atomic<const char *> network_status{"Joining Wi-Fi"};
esp_netif_t *wifi_netif = nullptr;
std::atomic<bool> requested{false};
std::atomic<bool> session_active{false};
std::atomic<bool> audio_diagnostic{false};
std::atomic<VoiceUiState> ui_state{VoiceUiState::Offline};
std::atomic<const char *> ui_detail{"Connecting to network"};
std::atomic<uint32_t> remaining_ms{0};
std::atomic<int> rtc_fault{0};
std::atomic<bool> rtc_reconnecting{false};
std::atomic<const char *> rtc_failure{nullptr};
std::atomic<uint32_t> main_loop_max_gap_ms{0};
TaskHandle_t s_control_task = nullptr;
uint64_t now_ms() { return esp_timer_get_time() / 1000; }

const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt-wdt";
        case ESP_RST_TASK_WDT: return "task-wdt";
        case ESP_RST_WDT: return "other-wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

void show(VoiceUiState state, const char *detail = nullptr) {
    ui_detail.store(detail);
    if (ui_state.exchange(state) != state) {
        ESP_LOGI(TAG, "UI state=%d", static_cast<int>(state));
    }
}

struct Agent { std::string id, name; };
using Library = std::vector<Agent>;
QueueHandle_t library_updates;
Library library;
size_t selected = 0;
enum class Page { Library, Settings, Volume, Device, User, Back };
Page page = Page::Library;
char device_id[13] = "Unavailable";
char start_id[128] = {};
std::atomic<bool> busy{false};
portMUX_TYPE lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> library_error{false};
std::atomic<bool> refresh_library{true};
bool library_loaded = false;
bool ptt_armed = false;
bool collect_agent(const char *id, const char *name, void *context) {
    if (strlen(id) >= sizeof(start_id)) return false;
    static_cast<Library *>(context)->push_back({id, name});
    return true;
}
void receive_library() {
    Library *update;
    if (!library_updates || xQueueReceive(library_updates, &update, 0) != pdTRUE) return;
    const std::string previous = selected < library.size() ? library[selected].id : "";
    library.swap(*update);
    delete update;
    auto it = std::find_if(library.begin(), library.end(), [&](const Agent &a){ return a.id == previous; });
    selected = it == library.end() ? 0 : static_cast<size_t>(it - library.begin());
    library_loaded = true;
}

void toggle_session(const char *source) {
    bool old = requested.load();
    if (!old) {
        if (!network_ready() || selected >= library.size() || busy.load()) return;
        strcpy(start_id, library[selected].id.c_str());
        ptt_armed = false;
        // End a local diagnostic before publishing the cloud start request.
        // The following control-task start therefore gets a fresh audio epoch.
        audio_diagnostic.store(false);
        voice_audio_set_active(false);
    }
    portENTER_CRITICAL(&lifecycle_lock);
    if (!old) busy.store(true);
    requested.store(!old);
    portEXIT_CRITICAL(&lifecycle_lock);
    if (old) {
        voice_audio_set_mic_held(false);
        // The control task may still be inside an HTTP operation. Mute local
        // capture/playout now; that task will finish cloud cleanup afterwards.
        voice_audio_set_active(false);
        session_active.store(false);
        remaining_ms.store(0);
        show(VoiceUiState::Ended, "Ending session");
    }
    ESP_LOGI(TAG, "%s: %s conversation", source, old ? "stop" : "start");
}

constexpr uint8_t kVolumeSteps[] = {0, 20, 40, 60, 80, 100};

int volume_index() {
    const uint8_t current = voice_audio_get_volume_percent();
    for (int i = 0; i < 6; ++i) {
        if (current <= kVolumeSteps[i]) return i;
    }
    return 5;
}

void cycle_volume() {
    const int index = volume_index();
    const int next = index == 0 ? 5 : index - 1;
    voice_audio_set_volume_percent(kVolumeSteps[next]);
    ESP_LOGI(TAG, "B: speaker volume=%u%%", unsigned(kVolumeSteps[next]));
}

// Short labels fit the Stick's settings screen and never contain network secrets.
std::atomic<const char *> wifi_status{"Joining Wi-Fi"};

const char *wifi_disconnect_label(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "No 2.4 GHz AP";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "Security mismatch";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_802_1X_AUTH_FAILED: return "Wi-Fi auth failed";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Handshake timeout";
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "Weak Wi-Fi signal";
        default: return "Wi-Fi disconnected";
    }
}

struct WifiProfile {
    const char *ssid;
    const char *password;
    const char *connected_label;
};
constexpr WifiProfile wifi_profiles[] = {
    {VOICE_WIFI_SSID, VOICE_WIFI_PASSWORD, "Event Wi-Fi"},
    {VOICE_WIFI_FALLBACK_SSID, VOICE_WIFI_FALLBACK_PASSWORD, "Hotspot Wi-Fi"},
};
enum class WifiPhase { Stopped, Joining, Dhcp, Disconnecting, Retry, Online };
ESP_EVENT_DEFINE_BASE(WIFI_RETRY_EVENT);
// These fields belong to the default ESP event loop. The timer only posts a
// tick, so an old DHCP timeout cannot race a lease acquired by the next profile.
WifiPhase wifi_phase = WifiPhase::Stopped;
unsigned wifi_profile = 0;
uint64_t wifi_deadline = 0;
bool has_fallback() {
    return wifi_profiles[1].ssid[0] && strcmp(wifi_profiles[0].ssid, wifi_profiles[1].ssid) != 0;
}
void schedule_wifi_retry() {
    if (has_fallback()) wifi_profile ^= 1;
    wifi_phase = WifiPhase::Retry;
    wifi_deadline = now_ms() + 1500;
}
void connect_wifi() {
    const auto &profile = wifi_profiles[wifi_profile];
    wifi_config_t config{};
    memcpy(config.sta.ssid, profile.ssid, strlen(profile.ssid));
    memcpy(config.sta.password, profile.password, strlen(profile.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.failure_retry_cnt = 1;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err == ESP_OK) {
        wifi_phase = WifiPhase::Joining;
        wifi_deadline = now_ms() + 30000;
        ESP_LOGI(TAG, "Joining %s", profile.connected_label);
    } else {
        ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(err));
        wifi_status.store("Wi-Fi setup failed");
        schedule_wifi_retry();
    }
}
void wifi_timer_tick(void *) {
    // If the event queue is full, the next periodic tick checks the same deadline.
    esp_event_post(WIFI_RETRY_EVENT, 0, nullptr, 0, 0);
}
void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_RETRY_EVENT) {
        if (now_ms() < wifi_deadline) return;
        if (wifi_phase == WifiPhase::Retry) {
            connect_wifi();
        } else if (wifi_phase == WifiPhase::Joining || wifi_phase == WifiPhase::Dhcp ||
                   wifi_phase == WifiPhase::Disconnecting) {
            if (wifi_phase == WifiPhase::Disconnecting) {
                wifi_ap_record_t access_point{};
                if (esp_wifi_sta_get_ap_info(&access_point) == ESP_ERR_WIFI_NOT_CONNECT) {
                    schedule_wifi_retry();
                    return;
                }
            } else {
                wifi_status.store(wifi_phase == WifiPhase::Dhcp ? "DHCP timed out" : "Join timed out");
                ESP_LOGW(TAG, "%s: %s", wifi_profiles[wifi_profile].connected_label, wifi_status.load());
            }
            wifi_phase = WifiPhase::Disconnecting;
            wifi_deadline = now_ms() + 3000;
            esp_err_t err = esp_wifi_disconnect();
            if (err == ESP_ERR_WIFI_NOT_CONNECT) schedule_wifi_retry();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_wifi();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && wifi_phase == WifiPhase::Joining) {
        wifi_phase = WifiPhase::Dhcp;
        wifi_deadline = now_ms() + 15000;
        wifi_status.store("Waiting for IP");
        ESP_LOGI(TAG, "Wi-Fi associated, waiting for DHCP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_ready.exchange(false)) ++wifi_generation;
        if (wifi_phase == WifiPhase::Stopped || wifi_phase == WifiPhase::Retry) return;
        if (wifi_phase != WifiPhase::Disconnecting) {
            const auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
            wifi_status.store(wifi_disconnect_label(event->reason));
            ESP_LOGW(TAG, "%s: %s (reason=%u)", wifi_profiles[wifi_profile].connected_label,
                     wifi_status.load(), unsigned(event->reason));
        }
        schedule_wifi_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP &&
               (wifi_phase == WifiPhase::Dhcp || wifi_phase == WifiPhase::Online)) {
        const auto *event = static_cast<ip_event_got_ip_t *>(data);
        if (event->esp_netif != wifi_netif) return;
        if (!wifi_ready.load() || event->ip_changed) ++wifi_generation;
        wifi_phase = WifiPhase::Online;
        wifi_status.store(wifi_profiles[wifi_profile].connected_label);
        wifi_ready.store(true);
        ESP_LOGI(TAG, "Wi-Fi connected via %s, DHCP lease acquired", wifi_status.load());
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP && wifi_phase == WifiPhase::Online) {
        wifi_phase = WifiPhase::Dhcp;
        wifi_deadline = now_ms() + 15000;
        if (wifi_ready.exchange(false)) ++wifi_generation;
        wifi_status.store("Waiting for IP");
    }
}

esp_err_t start_network_stack() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    return err == ESP_OK ? esp_event_loop_create_default() : err;
}

esp_err_t start_wifi() {
    wifi_config_t limits{};
    for (const auto &profile : wifi_profiles) {
        if (strlen(profile.ssid) > sizeof(limits.sta.ssid) ||
            strlen(profile.password) > sizeof(limits.sta.password)) return ESP_ERR_INVALID_ARG;
    }
    wifi_netif = esp_netif_create_default_wifi_sta();
    if (!wifi_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr),
                        TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr),
                        TAG, "IP events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_RETRY_EVENT, 0, wifi_event, nullptr),
                        TAG, "Wi-Fi retry events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi station mode");
    esp_timer_handle_t retry_timer;
    esp_timer_create_args_t timer_config{};
    timer_config.callback = wifi_timer_tick;
    timer_config.name = "wifi_retry";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_config, &retry_timer), TAG, "Wi-Fi retry timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(retry_timer, 500000), TAG, "Start Wi-Fi retry timer");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Start Wi-Fi");
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

void start_clock() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

struct NetworkRoute {
    NetworkLink link = NetworkLink::None;
    uint32_t generation = 0;
};

bool link_ready(NetworkLink link) {
    switch (link) {
        case NetworkLink::Wifi: return wifi_ready.load();
        case NetworkLink::Usb: return voice_usb_net_ready();
        default: return false;
    }
}

uint32_t link_generation(NetworkLink link) {
    return link == NetworkLink::Wifi ? wifi_generation.load() : voice_usb_net_generation();
}

bool route_ready(NetworkRoute route) {
    return link_ready(route.link) && link_generation(route.link) == route.generation;
}

esp_netif_t *link_netif(NetworkLink link) {
    return link == NetworkLink::Wifi ? wifi_netif : voice_usb_netif();
}

bool has_wall_clock() {
    return time(nullptr) > 1700000000;
}

bool set_default_route(NetworkLink link) {
    auto *netif = link_netif(link);
    return netif && esp_netif_set_default_netif(netif) == ESP_OK;
}

bool probe_service(NetworkRoute route) {
    if (!route_ready(route) || !set_default_route(route.link)) return false;
    struct ifreq interface{};
    if (esp_netif_get_netif_impl_name(link_netif(route.link), interface.ifr_name) != ESP_OK) return false;
    std::string url = VOICE_SERVER_URL;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/api/health";
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = 2000;
    config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    config.if_name = &interface;
    auto client = esp_http_client_init(&config);
    if (!client) return false;
    // Validate the small health response to reject a captive portal's
    // successful HTTP status, including when the configured app uses HTTP.
    char body[64]{};
    int length = -1;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200) {
        length = esp_http_client_read_response(client, body, sizeof(body) - 1);
    }
    const bool complete = length > 0 && length < sizeof(body) - 1 &&
        esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if (!complete || !route_ready(route)) return false;

    cJSON *result = cJSON_ParseWithLengthOpts(body, length + 1, nullptr, true);
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
    const bool reachable = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
    cJSON_Delete(result);
    return reachable;
}

void update_network(NetworkRoute &current, uint64_t &next_probe, NetworkLink &clock_link) {
    const uint64_t now = now_ms();
    if (!route_ready(current)) {
        active_link.store(NetworkLink::None);
        if (current.link != NetworkLink::None) next_probe = 0;
        current = {};
    }
    const bool have_wifi = link_ready(NetworkLink::Wifi);
    const bool have_usb = link_ready(NetworkLink::Usb);
    if (!have_wifi && !have_usb) {
        network_status.store(wifi_status.load());
        next_probe = 0;
        return;
    }

    if (!has_wall_clock()) {
        if (now >= next_probe || !link_ready(clock_link)) {
            // If SNTP is blocked on one link, let the other try with its DNS
            // and gateway. HTTPS probes still require certificate validation.
            clock_link = have_wifi && (!have_usb || clock_link != NetworkLink::Wifi)
                ? NetworkLink::Wifi : NetworkLink::Usb;
            if (set_default_route(clock_link)) esp_sntp_restart();
            next_probe = now + 15000;
        }
        network_status.store("Syncing clock");
        return;
    }
    if (clock_link != NetworkLink::None) {
        next_probe = 0;
        clock_link = NetworkLink::None;
    }
    if (now < next_probe) return;

    NetworkRoute selected_route{};
    for (const auto link : {NetworkLink::Wifi, NetworkLink::Usb}) {
        if (!link_ready(link)) continue;
        NetworkRoute candidate{link, link_generation(link)};
        if (probe_service(candidate)) {
            selected_route = candidate;
            break;
        }
    }
    if (selected_route.link != current.link) {
        ESP_LOGI(TAG, "App route: %s",
                 selected_route.link == NetworkLink::Usb ? "USB Ethernet" :
                 selected_route.link == NetworkLink::Wifi ? wifi_status.load() : "unavailable");
    }
    current = selected_route;
    active_link.store(current.link);
    network_status.store(current.link == NetworkLink::Usb ? "USB Ethernet" :
                         current.link == NetworkLink::Wifi ? wifi_status.load() : "App unreachable");
    next_probe = now_ms() + (network_ready() ? 15000 : 5000);
}

void transport_event(voice_transport_event_t event, int code, void *) {
    // SDK callbacks only publish atomics; logging can wait on the UART lock.
    switch (event) {
        case VOICE_EVENT_JOINED:
        case VOICE_EVENT_REJOINED:
            rtc_reconnecting.store(false);
            // The greeting can arrive while startAgent is still returning.
            // Push-to-talk keeps the microphone gated until the user holds A.
            if (requested.load()) voice_audio_set_active(true);
            break;
        case VOICE_EVENT_RECONNECTING: rtc_reconnecting.store(true); break;
        case VOICE_EVENT_REMOTE_OFFLINE:
        case VOICE_EVENT_CONNECTION_LOST:
        case VOICE_EVENT_TOKEN_EXPIRED:
        case VOICE_EVENT_UNSUPPORTED_AUDIO:
        case VOICE_EVENT_ERROR: {
            const char *reason = "RTC error";
            if (event == VOICE_EVENT_REMOTE_OFFLINE) reason = "Agent went offline";
            if (event == VOICE_EVENT_CONNECTION_LOST) reason = "RTC connection lost";
            if (event == VOICE_EVENT_TOKEN_EXPIRED) reason = "RTC token error";
            if (event == VOICE_EVENT_UNSUPPORTED_AUDIO) reason = "Unsupported audio codec";
            rtc_fault.store(code);
            rtc_failure.store(reason);
            break;
        }
        default: break;
    }
}

void receive_audio(voice_codec_t codec, const void *data, size_t size, uint32_t, void *) {
    if (!requested.load()) return;
    voice_audio_codec_t format;
    uint32_t rate = 16000;
    switch (codec) {
        case VOICE_CODEC_G722: format = VOICE_AUDIO_CODEC_G722; break;
        case VOICE_CODEC_PCMA: format = VOICE_AUDIO_CODEC_PCMA; rate = 8000; break;
        case VOICE_CODEC_PCM16: format = VOICE_AUDIO_CODEC_PCM16; break;
        default: return;
    }
    voice_audio_enqueue(format, data, size, rate);
}

esp_err_t send_audio(const uint8_t *data, size_t size, void *) {
    if (audio_diagnostic.load()) { return ESP_OK; } // Exercise capture without the network.
    if (!session_active.load()) return ESP_OK; // Greeting playback can precede agent-start acknowledgement.
    return voice_transport_send_audio(data, size, static_cast<uint32_t>(now_ms()));
}

void control_task(void *) {
    bool connected = false;
    bool cleanup_pending = false;
    uint64_t reconnect_since = 0;
    uint64_t cleanup_retry = 0;
    uint64_t audio_unavailable_since = 0;
    uint64_t startup = now_ms();
    uint64_t next_library = 0;
    uint64_t next_probe = 0;
    NetworkLink clock_link = NetworkLink::None;
    NetworkRoute route{};
    for (;;) {
        auto now = now_ms();
        if (requested.load() && !route_ready(route)) {
            requested.store(false);
            session_active.store(false);
            voice_audio_set_mic_held(false);
            voice_audio_set_active(false);
            show(VoiceUiState::Offline, "Network disconnected");
        }
        if (!connected) update_network(route, next_probe, clock_link);
        const bool app_ready = network_ready() && route_ready(route);
        if (!app_ready) next_library = 0;
        if (!busy.load() && app_ready &&
            (refresh_library.exchange(false) || now >= next_library) &&
            uxQueueMessagesWaiting(library_updates) == 0) {
            auto update = std::make_unique<Library>();
            esp_err_t err = voice_transport_library(VOICE_SERVER_URL, VOICE_DEVICE_KEY, device_id, collect_agent, update.get());
            library_error.store(err != ESP_OK);
            if (err != ESP_OK) next_probe = 0;
            if (err == ESP_OK) {
                Library *result = update.release();
                if (xQueueSend(library_updates, &result, 0) != pdTRUE) delete result;
            }
            const uint64_t finished = now_ms();
            next_library = finished + (err == ESP_OK ? 20000 : 5000);
            if (err == ESP_OK && route.link == NetworkLink::Wifi) {
                // A successful library poll also confirms the preferred route.
                next_probe = finished + 25000;
            }
        }
        if (!connected && !requested.load() && cleanup_pending && app_ready && now >= cleanup_retry) {
            cleanup_pending = voice_transport_stop() != ESP_OK;
            cleanup_retry = now_ms() + 5000;
        }
        if (!connected && requested.load() && app_ready && route_ready(route)) {
            show(VoiceUiState::Connecting);
            ESP_LOGI(TAG, "Session start heap: internal=%u largest=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            rtc_fault.store(0);
            rtc_reconnecting.store(false);
            rtc_failure.store(nullptr);
            // Do not start the microphone yet. RTSA initialization needs the
            // contiguous internal-RAM peak; the JOINED event enables audio
            // before startAgent is called.
            voice_transport_config_t config{};
            config.base_url = VOICE_SERVER_URL;
            config.device_key = VOICE_DEVICE_KEY;
            config.agent_id = start_id;
            config.codec = VOICE_CODEC_G722;
            config.area_code = VOICE_AREA_CODE;
            config.http_timeout_ms = 30000;
            config.join_timeout_ms = 15000;
            config.event_cb = transport_event;
            config.audio_cb = receive_audio;
            // HTTPS certificate verification needs a plausible wall clock.
            esp_err_t err = voice_transport_start(&config);
            if (err != ESP_OK) {
                voice_audio_set_active(false);
                requested.store(false);
                cleanup_pending = voice_transport_stop() != ESP_OK;
                cleanup_retry = now_ms() + 5000;
                if (err == ESP_ERR_HTTP_EAGAIN) show(VoiceUiState::Error, "Service timed out");
                else show(VoiceUiState::Error, "Connection failed");
                refresh_library.store(true);
                next_probe = 0;
                ESP_LOGW(TAG, "Session start: %s", esp_err_to_name(err));
            } else {
                connected = true;
                cleanup_pending = false;
                // A can cancel and restart while HTTP is in progress. Restore
                // audio to the latest requested state once startup completes.
                const bool wanted = requested.load() && route_ready(route);
                if (!wanted && requested.exchange(false)) {
                    show(VoiceUiState::Offline, "Network disconnected");
                }
                voice_audio_set_active(wanted);
                session_active.store(wanted);
                audio_unavailable_since = 0;
                if (wanted) show(VoiceUiState::Ready);
            }
        }

        if (connected && requested.load()) {
            now = now_ms();
            auto audio = voice_audio_get_status();
            if (const char *reason = rtc_failure.load()) {
                requested.store(false);
                ESP_LOGW(TAG, "Session ended: %s (code=%d)", reason, rtc_fault.load());
                show(VoiceUiState::Error, reason);
            } else if (rtc_reconnecting.load()) {
                if (reconnect_since == 0) reconnect_since = now;
                show(VoiceUiState::Connecting, "Reconnecting");
                if (now - reconnect_since >= 10000) {
                    requested.store(false);
                    show(VoiceUiState::Error, "Connection lost");
                }
            } else {
                reconnect_since = 0;
                show(audio.playing ? VoiceUiState::Speaking :
                     audio.user_speaking ? VoiceUiState::Listening : VoiceUiState::Ready);
            }
            if (audio.last_error != ESP_OK && !audio.recording && !audio.playing) {
                // Half-duplex switching deliberately has a short interval
                // with neither peripheral active. Do not treat an old send
                // error during initial RTC setup as a new hardware failure.
                if (audio_unavailable_since == 0) audio_unavailable_since = now;
                if (now - audio_unavailable_since > 4000) {
                    requested.store(false);
                    show(VoiceUiState::Error, "Audio error");
                }
            } else {
                audio_unavailable_since = 0;
            }
        }

        if (connected && !requested.load()) {
            voice_audio_set_active(false);
            session_active.store(false);
            auto state = ui_state.load();
            if (state != VoiceUiState::Ended && state != VoiceUiState::Error && state != VoiceUiState::Offline) {
                show(VoiceUiState::Ended);
            }
            remaining_ms.store(0);
            // After a link loss, select a reachable route for cloud cleanup.
            if (!route_ready(route)) update_network(route, next_probe, clock_link);
            cleanup_pending = voice_transport_stop() != ESP_OK;
            cleanup_retry = now_ms() + 5000;
            connected = false;
            reconnect_since = 0;
            ESP_LOGI(TAG, "Stopped; cloud cleanup=%s internal_free=%u", cleanup_pending ? "retry" : "done",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        }
        if (!connected) {
            portENTER_CRITICAL(&lifecycle_lock);
            if (!requested.load()) {
                voice_audio_set_mic_held(false);
                busy.store(false);
            }
            portEXIT_CRITICAL(&lifecycle_lock);
        }
        if (!connected && ui_state.load() == VoiceUiState::Offline && network_ready()) {
            show(VoiceUiState::Idle);
        }
        if (!network_ready() && !connected && now - startup > 15000) {
            show(VoiceUiState::Offline, network_status.load());
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void print_status() {
    const auto a = voice_audio_get_status();
    ESP_LOGI(TAG, "STATUS wifi=%d requested=%d session=%d rtc=%d state=%d volume=%u%% mic=%d spk=%d vad=%d rms=%u frames=%lu/%lu rx=%lu drop=%lu invalid=%lu pool=%lu stale=%lu playfail=%lu heap_internal=%u heap_largest=%u heap_min=%u usb=%d app=%d",
             wifi_ready.load(), requested.load(), session_active.load(), voice_transport_is_ready(),
             static_cast<int>(ui_state.load()), unsigned(a.volume_percent), a.recording, a.playing, a.user_speaking, a.mic_rms,
             static_cast<unsigned long>(a.sent_frames), static_cast<unsigned long>(a.captured_frames),
             static_cast<unsigned long>(a.received_packets), static_cast<unsigned long>(a.dropped_packets),
             static_cast<unsigned long>(a.dropped_invalid), static_cast<unsigned long>(a.dropped_pool_full),
             static_cast<unsigned long>(a.dropped_stale), static_cast<unsigned long>(a.dropped_playback),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             voice_usb_net_ready(), network_ready());
    ESP_LOGI(TAG, "AUDIO rx_bytes=%lu packet_bytes=%u decoded=%lu decoded_peak=%u played=%lu queue_peak=%u decode_us=%lu decode_max_us=%lu send_us=%lu send_max_us=%lu worker_gap_ms=%lu",
             static_cast<unsigned long>(a.received_bytes), unsigned(a.last_packet_bytes),
             static_cast<unsigned long>(a.decoded_samples), unsigned(a.decoded_peak),
             static_cast<unsigned long>(a.played_samples),
             unsigned(a.queue_high_water), static_cast<unsigned long>(a.decode_time_us),
             static_cast<unsigned long>(a.max_decode_us), static_cast<unsigned long>(a.send_time_us),
             static_cast<unsigned long>(a.max_send_us), static_cast<unsigned long>(a.max_worker_gap_ms));
    ESP_LOGI(TAG, "AUDIO playback_gap_ms=%lu control_stack_free=%u",
             static_cast<unsigned long>(a.max_playback_gap_ms),
             s_control_task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_control_task) * sizeof(StackType_t)) : 0);
    ESP_LOGI(TAG, "AUDIO_IO mic_begin_us=%lu mic_end_us=%lu spk_begin_us=%lu spk_end_us=%lu play_raw_us=%lu chunk_build_ms=%lu play=%lu/%lu starts mic=%lu spk=%lu queue=%u/%u",
             static_cast<unsigned long>(a.max_mic_begin_us), static_cast<unsigned long>(a.max_mic_end_us),
             static_cast<unsigned long>(a.max_speaker_begin_us), static_cast<unsigned long>(a.max_speaker_end_us),
             static_cast<unsigned long>(a.max_play_raw_us), static_cast<unsigned long>(a.max_chunk_build_ms),
             static_cast<unsigned long>(a.play_calls), static_cast<unsigned long>(a.play_failures),
             static_cast<unsigned long>(a.mic_starts), static_cast<unsigned long>(a.speaker_starts),
             unsigned(a.queued_packets), unsigned(a.free_packets));
    const auto ui = voice_ui_get_diagnostics();
    ESP_LOGI(TAG, "UI loop_gap_ms=%lu flush_us=%lu tick_us=%lu ticks=%lu",
             static_cast<unsigned long>(main_loop_max_gap_ms.load()),
             static_cast<unsigned long>(ui.max_flush_us), static_cast<unsigned long>(ui.max_tick_us),
             static_cast<unsigned long>(ui.rendered_frames));
}
} // namespace

extern "C" void app_main() {
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "M5 voice companion / IDF %s / reset=%d(%s)",
             esp_get_idf_version(), static_cast<int>(reset_reason), reset_reason_name(reset_reason));
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5StickS3;
    cfg.internal_mic = cfg.internal_spk = true;
    cfg.internal_imu = cfg.internal_rtc = false;
    cfg.external_display_value = 0;
    cfg.output_power = false;
    M5.begin(cfg);
    s_board_profile = voice_board_profile();
    if (s_board_profile.kind == VoiceBoardKind::CoreS3) M5.setTouchButtonHeight(36);
    M5.Display.setRotation(s_board_profile.rotation);
    M5.Display.setBrightness(160);
    M5.BtnA.setDebounceThresh(30);
    M5.BtnB.setDebounceThresh(30);
    s_board_profile.width = static_cast<uint16_t>(M5.Display.width());
    s_board_profile.height = static_cast<uint16_t>(M5.Display.height());
    ESP_LOGI(TAG, "BOARD=%s (%u) panel=%ux%u rotation=%u round=%d PSRAM=%u",
             s_board_profile.name, static_cast<unsigned>(M5.getBoard()),
             static_cast<unsigned>(s_board_profile.width), static_cast<unsigned>(s_board_profile.height),
             static_cast<unsigned>(s_board_profile.rotation), s_board_profile.round_display,
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
    ESP_LOGI(TAG, "Heap after M5.begin: internal=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    if (!voice_ui_init(s_board_profile)) {
        ESP_LOGE(TAG, "UI allocation failed");
        return;
    }
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK)
        snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    library_updates = xQueueCreate(1, sizeof(Library *));
    const bool stick = s_board_profile.kind == VoiceBoardKind::StickS3;
    const std::string_view ssid = VOICE_WIFI_SSID;
    const bool wifi_configured = !ssid.empty() && ssid != "CHANGE_ME";
    const std::string_view server = VOICE_SERVER_URL;
    const bool configured = !server.empty() && server.find("CHANGE_ME") == std::string_view::npos &&
        (wifi_configured || stick);
    if (!configured) {
        show(VoiceUiState::Error, stick ? "Set app URL" : "Set Wi-Fi and URL");
    } else {
        esp_err_t err = start_network_stack();
        if (err == ESP_OK) {
            if (wifi_configured) {
                const esp_err_t wifi_err = start_wifi();
                if (wifi_err != ESP_OK) {
                    wifi_status.store("Wi-Fi setup failed");
                    ESP_LOGW(TAG, "Wi-Fi startup: %s", esp_err_to_name(wifi_err));
                    if (!stick) err = wifi_err;
                }
            } else {
                wifi_status.store("Connect USB Ethernet");
            }
            if (stick) {
                const esp_err_t usb_err = voice_usb_net_start();
                if (usb_err != ESP_OK) {
                    ESP_LOGW(TAG, "USB Ethernet startup: %s", esp_err_to_name(usb_err));
                    if (!wifi_configured) err = usb_err;
                }
            }
        }
        if (err == ESP_OK) {
            start_clock();
            err = voice_audio_init(send_audio, nullptr);
        }
        if (err == ESP_OK && !library_updates) err = ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = xTaskCreate(control_task, "voice_control", 8192, nullptr, 3, &s_control_task) == pdPASS
                ? ESP_OK : ESP_ERR_NO_MEM;
        }
        if (err != ESP_OK) {
            show(VoiceUiState::Error, "Startup failed. Restart.");
        }
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // The Stick's USB OTG device uses the same internal PHY.
    if (!stick) {
        usb_serial_jtag_driver_config_t console_config{};
        console_config.tx_buffer_size = 1024;
        console_config.rx_buffer_size = 256;
        if (usb_serial_jtag_driver_install(&console_config) == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
        }
    }
#endif
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "A selects; hold A to talk; B selects next or exits chat; serial a=select, b=next, q=stop, s=status; diagnostic m=local mic toggle, p=60ms tone, h=heap trace, d=heap dump");
    uint64_t previous = now_ms();
    uint64_t last_status = previous;
    uint64_t last_power = 0;
    int battery = -1;
    bool charging = false;
    for (;;) {
        uint64_t now = now_ms();
        const uint32_t loop_gap_ms = static_cast<uint32_t>(now - previous);
        uint32_t previous_gap = main_loop_max_gap_ms.load(std::memory_order_relaxed);
        while (loop_gap_ms > previous_gap &&
               !main_loop_max_gap_ms.compare_exchange_weak(previous_gap, loop_gap_ms,
                                                           std::memory_order_relaxed)) {}
        M5.update();
        receive_library();
        char command = 0;
        read(STDIN_FILENO, &command, 1);
        const bool choose = M5.BtnA.wasPressed() || command == 'a';
        const bool next = (s_board_profile.has_secondary_button && M5.BtnB.wasPressed()) || command == 'b';
        if (requested.load()) {
            if (!M5.BtnA.isPressed()) ptt_armed = true;
            voice_audio_set_mic_held(ptt_armed && session_active.load() && M5.BtnA.isPressed());
            if (next) toggle_session("B");
        } else if (!busy.load()) {
            if (next) {
                switch (page) {
                case Page::Library:
                    if (selected + 1 < library.size()) ++selected;
                    else page = Page::Settings;
                    break;
                case Page::Settings: selected = 0; page = Page::Library; break;
                case Page::Volume: page = Page::Device; break;
                case Page::Device: page = Page::User; break;
                case Page::User: page = Page::Back; break;
                case Page::Back: page = Page::Volume; break;
                }
            } else if (choose) {
                if (page == Page::Settings) page = Page::Volume;
                else if (page == Page::Volume) cycle_volume();
                else if (page == Page::Back) page = Page::Library;
                else if (page == Page::Library && configured) {
                    if (library.empty()) refresh_library.store(true);
                    else toggle_session("A");
                }
            }
        }
        if (command) {
            if (configured) {
                if (command == 'q' && requested.load()) toggle_session("serial q");
                if (command == 's') print_status();
                if (!requested.load() && !session_active.load()) {
                    if (command == 'h') voice_heap_diag_start();
                    if (command == 'd') voice_heap_diag_dump();
                }
                if ((command == 'm' || command == 'p') && !requested.load() && !session_active.load()) {
                    if (command == 'm') {
                        const bool active = !voice_audio_get_status().active;
                        audio_diagnostic.store(active);
                        voice_audio_set_active(active);
                        ESP_LOGI(TAG, "DIAGNOSTIC local audio %s (no cloud)", active ? "started" : "stopped");
                    } else if (audio_diagnostic.load() && voice_audio_get_status().active) {
                        static int16_t tone[320];
                        for (size_t i = 0; i < 320; ++i) { tone[i] = (i % 32 < 16) ? 1200 : -1200; }
                        unsigned queued = 0;
                        for (int i = 0; i < 3; ++i) {
                            queued += voice_audio_enqueue(VOICE_AUDIO_CODEC_PCM16, tone, sizeof(tone), 16000);
                        }
                        ESP_LOGI(TAG, "DIAGNOSTIC 500Hz tone queued=%u/3 frames (60ms)", queued);
                    }
                }
            }
        }
        if (now - last_power > 2000) {
            last_power = now;
            battery = M5.Power.getBatteryLevel();
            charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
        }
        auto audio = voice_audio_get_status();
        VoiceUiSnapshot snapshot{};
        snapshot.state = ui_state.load();
        snapshot.voice_configured = configured;
        const auto link = active_link.load();
        snapshot.wifi_connected = link == NetworkLink::Wifi;
        snapshot.usb_connected = link == NetworkLink::Usb;
        snapshot.battery_percent = battery;
        snapshot.charging = charging;
        snapshot.remaining_ms = remaining_ms.load();
        snapshot.timeout_ms = VOICE_IDLE_TIMEOUT_MS;
        snapshot.audio_level = audio.playing ? audio.playback_level : audio.mic_level;
        snapshot.volume_percent = audio.volume_percent;
        snapshot.button_pressed = M5.BtnA.isPressed();
        snapshot.detail = ui_detail.load();
        char value[48] = {};
        if (requested.load()) {
            snapshot.action = "Hold A   B Exit";
            if (snapshot.state == VoiceUiState::Ready || snapshot.state == VoiceUiState::Listening ||
                snapshot.state == VoiceUiState::Thinking) {
                snapshot.title = M5.BtnA.isPressed() && ptt_armed ? "Listening" : "Hold A to talk";
                snapshot.detail = M5.BtnA.isPressed() && ptt_armed ? "Release to send" : "Microphone muted";
            }
        } else if (busy.load()) {
            snapshot.title = "Ending chat";
            snapshot.action = "Please wait";
        } else {
            switch (page) {
            case Page::Library:
                snapshot.title = library.empty() ? "My agents" : "Agent";
                snapshot.detail = !library.empty() ? library[selected].name.c_str() :
                    !network_ready() ? network_status.load() : library_error.load() ? "Sync failed" :
                    library_loaded ? "No agents installed" : "Syncing agents";
                snapshot.action = library.empty() ? "A Sync   B Next" : "A Open   B Next";
                if (!library.empty() && library_error.load()) snapshot.title = "Sync failed";
                if (!network_ready()) {
                    snapshot.title = "Offline";
                    snapshot.detail = network_status.load();
                    snapshot.action = "B Next";
                }
                if (ui_state.load() == VoiceUiState::Error && ui_detail.load()) snapshot.title = ui_detail.load();
                break;
            case Page::Settings:
                snapshot.title = "Settings"; snapshot.detail = network_status.load();
                snapshot.action = "A Open   B Next"; break;
            case Page::Volume:
                snprintf(value, sizeof(value), "%u%%", unsigned(audio.volume_percent));
                snapshot.title = "Volume"; snapshot.detail = value;
                snapshot.action = "A Level   B Next"; break;
            case Page::Device:
                snapshot.title = "Device ID"; snapshot.detail = device_id;
                snapshot.action = "B Next"; break;
            case Page::User:
                snapshot.title = "User"; snapshot.detail = "Hermes (simulated)";
                snapshot.action = "B Next"; break;
            case Page::Back:
                snapshot.title = "Back"; snapshot.detail = "Return to my agents";
                snapshot.action = "A Back   B Next"; break;
            }
        }
        voice_ui_update(snapshot);
        voice_ui_tick(static_cast<uint32_t>(now - previous));
        previous = now;
        if (now - last_status >= 5000) { last_status = now; print_status(); }
        vTaskDelay(pdMS_TO_TICKS(s_board_profile.round_display ? 1 : 5));
    }
}
