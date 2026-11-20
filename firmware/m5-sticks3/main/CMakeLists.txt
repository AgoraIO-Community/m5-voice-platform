set(BASE "${CMAKE_CURRENT_LIST_DIR}/..")
idf_component_register(
    SRCS "voice_ui.cpp" "main.cpp" "board_profile.cpp" "voice_audio.cpp" "voice_transport.c" "voice_usb_net.c" "voice_rtsa_time_compat.c" "voice_heap_diag.c"
         "voice_ui_font_12.c" "voice_ui_font_18.c"
         "${BASE}/src/app_atomic_compat.c" "${BASE}/src/app_rtsa_compat.c"
         "${BASE}/src/app_codec_g711.c"
         "${BASE}/src/app_vendor_g722_decode.c" "${BASE}/src/app_vendor_g722_encode.c"
    INCLUDE_DIRS "." "${BASE}/src" "${BASE}/third_party/agora_rtsa_sdk/agora_sdk/include"
    REQUIRES M5Unified M5GFX lvgl esp_wifi esp_netif esp_event nvs_flash
             esp_timer esp_http_client mbedtls espressif__cjson esp_driver_usb_serial_jtag
)
target_link_directories(${COMPONENT_LIB} PUBLIC "${BASE}/third_party/agora_rtsa_sdk/agora_sdk/lib/xtensa_lx7")
target_link_libraries(${COMPONENT_LIB} PUBLIC agora-rtc-sdk aosl)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition"
    "-Wl,--wrap=aosl_hal_cond_timedwait"
    "-Wl,-u,__wrap_aosl_hal_cond_timedwait"
    "-Wl,-u,_ctype_" "-Wl,-u,ahpl_bswap_32" "-Wl,-u,ahpl_bswap_64"
    "-Wl,-u,xTaskCreateRestrictedPinnedToCore")
