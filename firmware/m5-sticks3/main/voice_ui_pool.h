#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL's C allocator hook. voice_ui_init() must reserve this lifetime-long pool
// and check allocation before calling lv_init(); the hook never allocates.
void *voice_ui_pool_get(size_t size);

#ifdef __cplusplus
}
#endif
