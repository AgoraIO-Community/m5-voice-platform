#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_CUSTOM 0
// The complete two-board UI uses about 10 KiB of LVGL objects in the native
// simulator. 16 KiB leaves useful text/style headroom for the device layouts.
#define LV_MEM_SIZE (16U * 1024U)
#ifdef ESP_PLATFORM
// Reserve the fixed TLSF pool in PSRAM before LVGL starts when it exists; the
// allocator still falls back to internal RAM if PSRAM is unavailable.
#define LV_MEM_POOL_INCLUDE "voice_ui_pool.h"
#define LV_MEM_POOL_ALLOC(size) voice_ui_pool_get(size)
#endif
#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_INDEV_DEF_READ_PERIOD 20
#define LV_TICK_CUSTOM 0
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif
