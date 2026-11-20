#pragma once
#if __has_include("voice_config_local.h")
#include "voice_config_local.h"
#endif
#ifndef VOICE_WIFI_SSID
#define VOICE_WIFI_SSID "CHANGE_ME"
#endif
#ifndef VOICE_WIFI_PASSWORD
#define VOICE_WIFI_PASSWORD "CHANGE_ME"
#endif
#ifndef VOICE_WIFI_FALLBACK_SSID
#define VOICE_WIFI_FALLBACK_SSID ""
#endif
#ifndef VOICE_WIFI_FALLBACK_PASSWORD
#define VOICE_WIFI_FALLBACK_PASSWORD ""
#endif
#ifndef VOICE_SERVER_URL
#define VOICE_SERVER_URL "http://CHANGE_ME:8001"
#endif
#ifndef VOICE_DEVICE_KEY
#define VOICE_DEVICE_KEY ""
#endif
#ifndef VOICE_IDLE_TIMEOUT_MS
#define VOICE_IDLE_TIMEOUT_MS 15000U
#endif
#ifndef VOICE_RESPONSE_TIMEOUT_MS
#define VOICE_RESPONSE_TIMEOUT_MS 30000U
#endif
#ifndef VOICE_AREA_CODE
/* AREA_CODE_GLOB (0xFFFFFFFF): join all media zones. Use 0x00000001U (AREA_CODE_CN)
 * in voice_config_local.h when AGORA_AREA=cn / Shengwang.cn. */
#define VOICE_AREA_CODE 0xFFFFFFFFU
#endif
