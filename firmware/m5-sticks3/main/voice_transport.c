#include "voice_transport.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agora_rtc_api.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Service responses contain only a token/config or agent id; 16 KiB also
 * covers a verbose error body without exposing it in logs. */
#define HTTP_BODY_LIMIT 16384U
#define URL_LIMIT 512U
#define PATH_LIMIT 128U
#define TOKEN_LIMIT 4096U
#define DEVICE_KEY_LIMIT 128U
#define BEARER_PREFIX "Bearer "

typedef struct {
    char *data;
    size_t used;
    esp_err_t error;
} http_body_t;

typedef struct {
    char app_id[80];
    char token[TOKEN_LIMIT];
    char channel[80];
    uint32_t uid;
    uint32_t agent_uid;
    char agent_id[128];
} session_credentials_t;

typedef struct {
    voice_transport_config_t config;
    char base_url[URL_LIMIT];
    char authorization[sizeof(BEARER_PREFIX) + DEVICE_KEY_LIMIT];
    char get_path[PATH_LIMIT];
    char start_path[PATH_LIMIT];
    char stop_path[PATH_LIMIT];
    session_credentials_t credentials;
    connection_id_t connection;
    bool sdk_initialized;
    char engine_app_id[80];
    uint32_t engine_area_code;
    bool connection_created;
    bool join_requested;
    atomic_bool callbacks_enabled;
    atomic_bool joined;
    atomic_bool agent_started;
    atomic_bool token_expired;
    atomic_bool unsupported_audio_reported;
    atomic_int join_error;
} transport_t;

static const char *TAG = "voice_transport";
static transport_t s_transport;
/* Initialized by the control task before capture starts; retained for the
 * process lifetime so a late capture call never observes a deleted mutex. */
static SemaphoreHandle_t s_send_mutex;

static void wipe_memory(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size-- != 0) {
        *p++ = 0;
    }
}

static esp_err_t copy_string(char *dest, size_t capacity, const char *source)
{
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strlen(source);
    if (length >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dest, source, length + 1);
    return ESP_OK;
}

static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    http_body_t *body = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    if (body->error != ESP_OK) {
        return body->error;
    }
    if ((size_t)event->data_len > HTTP_BODY_LIMIT - body->used) {
        body->error = ESP_ERR_INVALID_SIZE;
        return body->error;
    }
    memcpy(body->data + body->used, event->data, event->data_len);
    body->used += event->data_len;
    body->data[body->used] = '\0';
    return ESP_OK;
}

/* Never log URLs, JSON bodies, or SDK error strings: each can contain tokens. */
static esp_err_t http_json_at(const char *base_url, const char *authorization, const char *device_id, int timeout_ms,
                              esp_http_client_method_t method, const char *path,
                           const cJSON *request, cJSON **response)
{
    *response = NULL;
    size_t url_size = strlen(base_url) + strlen(path) + 1;
    char *url = malloc(url_size);
    http_body_t body = { .data = calloc(1, HTTP_BODY_LIMIT + 1) };
    char *request_text = request != NULL ? cJSON_PrintUnformatted(request) : NULL;
    if (url == NULL || body.data == NULL || (request != NULL && request_text == NULL)) {
        free(url);
        free(body.data);
        cJSON_free(request_text);
        return ESP_ERR_NO_MEM;
    }
    snprintf(url, url_size, "%s%s", base_url, path);
    esp_http_client_config_t http_config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = on_http_event,
        .user_data = &body,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_err_t err = client != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    int status = 0;
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Accept", "application/json");
    }
    if (err == ESP_OK && authorization[0] != '\0') {
        err = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (err == ESP_OK && device_id != NULL) {
        err = esp_http_client_set_header(client, "X-Device-Id", device_id);
    }
    if (err == ESP_OK && request_text != NULL) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK) {
            err = esp_http_client_set_post_field(client, request_text, strlen(request_text));
        }
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (body.error != ESP_OK) {
        err = body.error;
    }
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG, "Control service returned HTTP %d", status);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        /* Reject truncated, malformed, and trailing non-JSON response data. */
        *response = cJSON_ParseWithLengthOpts(body.data, body.used + 1, NULL, true);
        if (!cJSON_IsObject(*response)) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(*response, "code");
            if (code != NULL && (!cJSON_IsNumber(code) || code->valuedouble != 0)) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (err != ESP_OK) {
        cJSON_Delete(*response);
        *response = NULL;
    }
    wipe_memory(body.data, body.used);
    free(body.data);
    cJSON_free(request_text);
    free(url);
    return err;
}

static esp_err_t http_json(esp_http_client_method_t method, const char *path,
                           const cJSON *request, cJSON **response)
{
    return http_json_at(s_transport.base_url, s_transport.authorization, NULL,
                        s_transport.config.http_timeout_ms, method, path, request, response);
}

esp_err_t voice_transport_library(const char *base_url, const char *device_key, const char *device_id,
                                  voice_library_item_cb item, void *context)
{
    if (!base_url || strncmp(base_url, "https://", 8) || !device_key || !*device_key ||
        strlen(device_key) > DEVICE_KEY_LIMIT || strpbrk(device_key, " \t\r\n") || !item)
        return ESP_ERR_INVALID_ARG;
    char authorization[sizeof(BEARER_PREFIX) + DEVICE_KEY_LIMIT];
    snprintf(authorization, sizeof(authorization), BEARER_PREFIX "%s", device_key);
    cJSON *response = NULL;
    const char *path = base_url[strlen(base_url)-1] == '/' ? "device_agents" : "/device_agents";
    esp_err_t err = http_json_at(base_url, authorization, device_id, 5000, HTTP_METHOD_GET, path, NULL, &response);
    wipe_memory(authorization, sizeof(authorization));
    if (err != ESP_OK) return err;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    const cJSON *account = cJSON_GetObjectItemCaseSensitive(data, "account");
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(data, "agents");
    if (!cJSON_IsArray(agents) || !cJSON_IsString(account) || strcmp(account->valuestring, "hermes")) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else {
        const cJSON *agent;
        cJSON_ArrayForEach(agent, agents) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(agent, "id");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(agent, "name");
            if (!cJSON_IsString(id) || !*id->valuestring || !cJSON_IsString(name) ||
                !*name->valuestring || !item(id->valuestring, name->valuestring, context)) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
    }
    cJSON_Delete(response);
    return err;
}

static esp_err_t json_string(const cJSON *object, const char *key, char *dest, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return copy_string(dest, size, item->valuestring);
}

static esp_err_t json_uid(const cJSON *object, const char *key, uint32_t *uid)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        double number = item->valuedouble;
        if (!(number >= 1 && number <= UINT32_MAX) || number != (double)(uint32_t)number) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *uid = (uint32_t)number;
        return ESP_OK;
    }
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (const char *p = item->valuestring; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(item->valuestring, &end, 10);
    if (errno == ERANGE || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *uid = (uint32_t)parsed;
    return ESP_OK;
}

static esp_err_t fetch_credentials(void)
{
    cJSON *response = NULL;
    esp_err_t err = http_json(HTTP_METHOD_GET, s_transport.get_path, NULL, &response);
    if (err != ESP_OK) {
        return err;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (!cJSON_IsObject(data)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    session_credentials_t *c = &s_transport.credentials;
    if (err == ESP_OK) err = json_string(data, "app_id", c->app_id, sizeof(c->app_id));
    if (err == ESP_OK) err = json_string(data, "token", c->token, sizeof(c->token));
    if (err == ESP_OK) err = json_string(data, "channel_name", c->channel, sizeof(c->channel));
    if (err == ESP_OK) err = json_uid(data, "uid", &c->uid);
    if (err == ESP_OK) err = json_uid(data, "agent_uid", &c->agent_uid);
    if (err == ESP_OK && c->uid == c->agent_uid) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC UIDs: Stick=%lu agent=%lu", (unsigned long)c->uid,
                 (unsigned long)c->agent_uid);
    }
    cJSON_Delete(response);
    return err;
}

static esp_err_t start_agent(void)
{
    cJSON *request = cJSON_CreateObject();
    cJSON *response = NULL;
    cJSON *parameters = request != NULL ? cJSON_AddObjectToObject(request, "parameters") : NULL;
    session_credentials_t *c = &s_transport.credentials;
    if (parameters == NULL ||
        (s_transport.config.agent_id && !cJSON_AddStringToObject(request, "agentId", s_transport.config.agent_id)) ||
        cJSON_AddStringToObject(request, "channelName", c->channel) == NULL ||
        cJSON_AddNumberToObject(request, "rtcUid", c->agent_uid) == NULL ||
        cJSON_AddNumberToObject(request, "userUid", c->uid) == NULL ||
        cJSON_AddStringToObject(parameters, "output_audio_codec",
                                s_transport.config.codec == VOICE_CODEC_G722 ? "g722" : "pcma") == NULL) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = http_json(HTTP_METHOD_POST, s_transport.start_path, request, &response);
    if (err == ESP_OK) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
        err = json_string(data, "agent_id", c->agent_id, sizeof(c->agent_id));
    }
    cJSON_Delete(response);
    cJSON_Delete(request);
    if (err == ESP_OK) {
        atomic_store(&s_transport.agent_started, true);
    }
    return err;
}

static esp_err_t stop_agent(void)
{
    if (s_transport.credentials.agent_id[0] == '\0') {
        return ESP_OK;
    }
    cJSON *request = cJSON_CreateObject();
    cJSON *response = NULL;
    if (request == NULL || cJSON_AddStringToObject(request, "agentId", s_transport.credentials.agent_id) == NULL) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = http_json(HTTP_METHOD_POST, s_transport.stop_path, request, &response);
    cJSON_Delete(response);
    cJSON_Delete(request);
    if (err == ESP_OK) {
        wipe_memory(s_transport.credentials.agent_id, sizeof(s_transport.credentials.agent_id));
    }
    return err;
}

static bool accept_callback(connection_id_t connection)
{
    return atomic_load(&s_transport.callbacks_enabled) && connection == s_transport.connection;
}

static void emit_event(voice_transport_event_t event, int code)
{
    if (s_transport.config.event_cb != NULL) {
        s_transport.config.event_cb(event, code, s_transport.config.ctx);
    }
}

static void on_joined(connection_id_t connection, uint32_t uid, int elapsed_ms)
{
    (void)uid;
    (void)elapsed_ms;
    if (accept_callback(connection)) {
        atomic_store(&s_transport.joined, true);
        emit_event(VOICE_EVENT_JOINED, 0);
    }
}

static void on_rejoined(connection_id_t connection, uint32_t uid, int elapsed_ms)
{
    (void)uid;
    (void)elapsed_ms;
    if (accept_callback(connection)) {
        atomic_store(&s_transport.joined, true);
        emit_event(VOICE_EVENT_REJOINED, 0);
    }
}

static void on_reconnecting(connection_id_t connection)
{
    if (accept_callback(connection)) {
        atomic_store(&s_transport.joined, false);
        emit_event(VOICE_EVENT_RECONNECTING, 0);
    }
}

static void on_connection_lost(connection_id_t connection)
{
    if (accept_callback(connection)) {
        atomic_store(&s_transport.joined, false);
        emit_event(VOICE_EVENT_CONNECTION_LOST, 0);
    }
}

static void on_error(connection_id_t connection, int code, const char *message)
{
    (void)message;
    if (!accept_callback(connection)) {
        return;
    }
    atomic_store(&s_transport.joined, false);
    atomic_store(&s_transport.join_error, code != 0 ? code : -1);
    if (code == ERR_TOKEN_EXPIRED || code == -ERR_TOKEN_EXPIRED ||
        code == ERR_INVALID_TOKEN || code == -ERR_INVALID_TOKEN) {
        atomic_store(&s_transport.token_expired, true);
        emit_event(VOICE_EVENT_TOKEN_EXPIRED, code);
    } else {
        emit_event(VOICE_EVENT_ERROR, code);
    }
}

static void on_token_expiring(connection_id_t connection, const char *token)
{
    (void)token;
    if (accept_callback(connection)) {
        atomic_store(&s_transport.token_expired, true);
        emit_event(VOICE_EVENT_TOKEN_EXPIRED, ERR_TOKEN_EXPIRED);
    }
}

static void on_license_failed(connection_id_t connection, int error)
{
    if (accept_callback(connection)) {
        atomic_store(&s_transport.joined, false);
        atomic_store(&s_transport.join_error, error != 0 ? error : -1);
        emit_event(VOICE_EVENT_ERROR, error);
    }
}

static void on_remote_joined(connection_id_t connection, uint32_t uid, int elapsed_ms)
{
    if (accept_callback(connection) && uid == s_transport.credentials.agent_uid) {
        ESP_LOGI(TAG, "Agent joined RTC: uid=%lu elapsed_ms=%d", (unsigned long)uid, elapsed_ms);
        emit_event(VOICE_EVENT_REMOTE_JOINED, 0);
    }
}

static void on_remote_offline(connection_id_t connection, uint32_t uid, int reason)
{
    if (accept_callback(connection) && uid == s_transport.credentials.agent_uid) {
        emit_event(VOICE_EVENT_REMOTE_OFFLINE, reason);
    }
}

static void on_audio(connection_id_t connection, uint32_t uid, uint16_t sent_ts,
                     const void *data, size_t size, const audio_frame_info_t *info)
{
    if (!accept_callback(connection) || uid != s_transport.credentials.agent_uid ||
        data == NULL || size == 0 || info == NULL || s_transport.config.audio_cb == NULL ||
        atomic_load(&s_transport.token_expired)) {
        return;
    }
    voice_codec_t codec;
    switch (info->data_type) {
    case AUDIO_DATA_TYPE_G722: codec = VOICE_CODEC_G722; break;
    case AUDIO_DATA_TYPE_PCMA: codec = VOICE_CODEC_PCMA; break;
    case AUDIO_DATA_TYPE_PCM: codec = VOICE_CODEC_PCM16; break;
    default:
        if (!atomic_exchange(&s_transport.unsupported_audio_reported, true)) {
            emit_event(VOICE_EVENT_UNSUPPORTED_AUDIO, info->data_type);
        }
        return;
    }
    s_transport.config.audio_cb(codec, data, size, sent_ts, s_transport.config.ctx);
}

/* The mutex excludes in-flight sends; SDK callbacks never acquire it. */
static esp_err_t release_rtc(void)
{
    atomic_store(&s_transport.callbacks_enabled, false);
    atomic_store(&s_transport.joined, false);
    atomic_store(&s_transport.agent_started, false);
    if (s_send_mutex == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (s_transport.connection_created) {
        if (s_transport.join_requested) {
            int code = agora_rtc_leave_channel(s_transport.connection);
            if (code < 0 && code != -ERR_NOT_IN_CHANNEL) result = ESP_FAIL;
            s_transport.join_requested = false;
        }
        if (agora_rtc_destroy_connection(s_transport.connection) < 0) {
            /* Keep the handle so a later stop retries destruction instead
             * of silently abandoning a connection inside the shared engine. */
            result = ESP_FAIL;
        } else {
            s_transport.connection_created = false;
        }
    }
    /* RTSA 1.9.7 documents one engine init per process. On this archive,
     * repeated init/fini leaks an AOSL DNS worker (4 KiB stack, TCB, locks and
     * TLS; confirmed by standalone heap tracing). Keep that engine for the
     * device lifetime. Every session still destroys its RTC connection and
     * stops its cloud Agent; only process-wide SDK resources remain. */
    xSemaphoreGive(s_send_mutex);
    wipe_memory(s_transport.credentials.token, sizeof(s_transport.credentials.token));
    return result;
}

esp_err_t voice_transport_stop(void)
{
    /* Release local audio/RTC immediately, before a potentially slow HTTP
     * request. Preserve an unacknowledged agent ID for the next retry. */
    esp_err_t rtc_err = release_rtc();
    esp_err_t http_err = stop_agent();
    return http_err != ESP_OK ? http_err : rtc_err;
}

esp_err_t voice_transport_start(const voice_transport_config_t *config)
{
    if (config == NULL || config->base_url == NULL ||
        (config->codec != VOICE_CODEC_G722 && config->codec != VOICE_CODEC_PCMA) ||
        config->http_timeout_ms > INT_MAX || config->join_timeout_ms > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool https = strncmp(config->base_url, "https://", 8) == 0;
    if (!https && strncmp(config->base_url, "http://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (https) return ESP_ERR_NOT_SUPPORTED;
#endif
    const char *key = config->device_key;
    const size_t key_length = key != NULL ? strlen(key) : 0;
    if (key_length != 0 &&
        (!https || key_length > DEVICE_KEY_LIMIT || strpbrk(key, " \t\r\n") != NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_transport.connection_created || atomic_load(&s_transport.agent_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A previous network failure must not silently orphan a cloud agent. */
    if (s_transport.credentials.agent_id[0] != '\0') {
        esp_err_t pending = voice_transport_stop();
        if (pending != ESP_OK) return pending;
    }
    if (s_send_mutex == NULL) {
        s_send_mutex = xSemaphoreCreateMutex();
        if (s_send_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    const char *get_path = config->get_config_path != NULL ? config->get_config_path : "/get_config";
    const char *start_path = config->start_agent_path != NULL ? config->start_agent_path : "/startAgent";
    const char *stop_path = config->stop_agent_path != NULL ? config->stop_agent_path : "/stopAgent";
    if (get_path[0] != '/' || start_path[0] != '/' || stop_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = copy_string(s_transport.base_url, sizeof(s_transport.base_url), config->base_url);
    if (err == ESP_OK) err = copy_string(s_transport.get_path, sizeof(s_transport.get_path), get_path);
    if (err == ESP_OK) err = copy_string(s_transport.start_path, sizeof(s_transport.start_path), start_path);
    if (err == ESP_OK) err = copy_string(s_transport.stop_path, sizeof(s_transport.stop_path), stop_path);
    if (err != ESP_OK) return err;
    size_t base_len = strlen(s_transport.base_url);
    while (base_len > 0 && s_transport.base_url[base_len - 1] == '/') {
        s_transport.base_url[--base_len] = '\0';
    }
    s_transport.config = *config;
    s_transport.config.base_url = s_transport.base_url;
    s_transport.config.device_key = NULL;
    wipe_memory(s_transport.authorization, sizeof(s_transport.authorization));
    if (key_length != 0) {
        snprintf(s_transport.authorization, sizeof(s_transport.authorization), BEARER_PREFIX "%s", key);
    }
    s_transport.config.get_config_path = s_transport.get_path;
    s_transport.config.start_agent_path = s_transport.start_path;
    s_transport.config.stop_agent_path = s_transport.stop_path;
    if (s_transport.config.http_timeout_ms == 0) s_transport.config.http_timeout_ms = 15000;
    if (s_transport.config.join_timeout_ms == 0) s_transport.config.join_timeout_ms = 15000;
    wipe_memory(&s_transport.credentials, sizeof(s_transport.credentials));
    atomic_store(&s_transport.join_error, 0);
    atomic_store(&s_transport.joined, false);
    atomic_store(&s_transport.token_expired, false);
    atomic_store(&s_transport.unsupported_audio_reported, false);
    err = fetch_credentials();
    if (err != ESP_OK) goto failed;

    const uint32_t area_code = config->area_code == 0 ? AREA_CODE_GLOB : config->area_code;
    if (s_transport.sdk_initialized &&
        (strcmp(s_transport.engine_app_id, s_transport.credentials.app_id) != 0 ||
         s_transport.engine_area_code != area_code)) {
        ESP_LOGE(TAG, "RTC App ID or region changed; restart the device before using the new configuration");
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }

    agora_rtc_event_handler_t handler = {
        .on_join_channel_success = on_joined,
        .on_rejoin_channel_success = on_rejoined,
        .on_reconnecting = on_reconnecting,
        .on_connection_lost = on_connection_lost,
        .on_error = on_error,
        .on_token_privilege_will_expire = on_token_expiring,
        .on_license_validation_failure = on_license_failed,
        .on_user_joined = on_remote_joined,
        .on_user_offline = on_remote_offline,
        .on_audio_data = on_audio,
    };
    int code;
    if (!s_transport.sdk_initialized) {
        rtc_service_option_t service = { .area_code = area_code };
        service.log_cfg.log_level = RTC_LOG_ERROR;
        service.log_cfg.log_disable = true;
        service.log_cfg.log_path = "io.agora.m5stack";
        ESP_LOGI(TAG, "RTSA init heap: internal=%u largest=%u dma_largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        code = agora_rtc_init(s_transport.credentials.app_id, &handler, &service);
        if (code < 0) {
            err = ESP_FAIL;
            goto failed;
        }
        s_transport.sdk_initialized = true;
        memcpy(s_transport.engine_app_id, s_transport.credentials.app_id, sizeof(s_transport.engine_app_id));
        s_transport.engine_area_code = area_code;
        ESP_LOGI(TAG, "RTSA initialized; SDK heap used=%d KiB", agora_rtc_get_memory_used());
    }
    code = agora_rtc_create_connection(&s_transport.connection);
    if (code < 0) {
        err = ESP_FAIL;
        goto failed;
    }
    s_transport.connection_created = true;
    rtc_channel_options_t options = {
        .auto_subscribe_audio = true,
        .auto_subscribe_video = false,
        /* RTSA 1.9.7's jitter output is PCM, not transparent compressed
         * packets. Keep the external G.722/PCMA decoder path consistent;
         * the application audio worker owns bounded playout buffering. */
        .enable_audio_jitter_buffer = false,
        .enable_audio_mixer = false,
        .enable_audio_decode = false,
        .audio_codec_opt = {
            .audio_codec_type = AUDIO_CODEC_DISABLED,
            .pcm_sample_rate = config->codec == VOICE_CODEC_G722 ? 16000 : 8000,
            .pcm_channel_num = 1,
            .pcm_duration = 20,
        },
    };
    atomic_store(&s_transport.callbacks_enabled, true);
    s_transport.join_requested = true;
    code = agora_rtc_join_channel(s_transport.connection, s_transport.credentials.channel,
                                  s_transport.credentials.uid, s_transport.credentials.token, &options);
    if (code < 0) {
        err = ESP_FAIL;
        goto failed;
    }
    TickType_t began = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(s_transport.config.join_timeout_ms);
    if (timeout == 0) timeout = 1;
    while (!atomic_load(&s_transport.joined)) {
        if (atomic_load(&s_transport.join_error) != 0 || atomic_load(&s_transport.token_expired)) {
            err = ESP_FAIL;
            goto failed;
        }
        if ((TickType_t)(xTaskGetTickCount() - began) >= timeout) {
            err = ESP_ERR_TIMEOUT;
            goto failed;
        }
        vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
    }
    err = start_agent();
    if (err != ESP_OK) goto failed;
    if (atomic_load(&s_transport.token_expired) || atomic_load(&s_transport.join_error) != 0) {
        err = ESP_FAIL;
        goto failed;
    }
    ESP_LOGI(TAG, "Voice session started (%s)", config->codec == VOICE_CODEC_G722 ? "G722/16k" : "PCMA/8k");
    return ESP_OK;

failed:
    (void)voice_transport_stop();
    ESP_LOGW(TAG, "Voice session start failed: %s", esp_err_to_name(err));
    return err;
}

bool voice_transport_is_ready(void)
{
    return atomic_load(&s_transport.callbacks_enabled) && atomic_load(&s_transport.joined) &&
           atomic_load(&s_transport.agent_started) && !atomic_load(&s_transport.token_expired);
}

esp_err_t voice_transport_send_audio(const void *data, size_t size, uint32_t pts_ms)
{
    (void)pts_ms;
    if (data == NULL || size == 0) return ESP_ERR_INVALID_ARG;
    if (!voice_transport_is_ready() || s_send_mutex == NULL) return ESP_ERR_INVALID_STATE;
    /* Capture must not stall behind a disconnect or HTTP control operation. */
    if (xSemaphoreTake(s_send_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (voice_transport_is_ready()) {
        audio_frame_info_t info = {
            .data_type = s_transport.config.codec == VOICE_CODEC_G722 ? AUDIO_DATA_TYPE_G722 : AUDIO_DATA_TYPE_PCMA,
        };
        int result = agora_rtc_send_audio_data(s_transport.connection, data, size, &info);
        err = result >= 0 ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_send_mutex);
    return err;
}
