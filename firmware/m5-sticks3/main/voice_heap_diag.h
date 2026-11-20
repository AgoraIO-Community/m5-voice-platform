#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional serial diagnostics. Call serially from the command task, never
 * from an SDK/audio callback. Warm up one complete session, then start the
 * trace, run another session, stop it and wait at least 5 seconds before dump.
 * Disabled builds return ESP_ERR_NOT_SUPPORTED and allocate no memory. */
esp_err_t voice_heap_diag_start(void);
esp_err_t voice_heap_diag_dump(void);

#ifdef __cplusplus
}
#endif
