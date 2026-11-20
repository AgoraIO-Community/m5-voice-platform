#include "voice_heap_diag.h"

#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "voice_heap";

#if CONFIG_HEAP_TRACING_STANDALONE
#include <stdbool.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_memory_utils.h"

#ifndef VOICE_HEAP_TRACE_RECORDS
#define VOICE_HEAP_TRACE_RECORDS 768
#endif

static heap_trace_record_t *s_records;
static heap_trace_record_t s_idle_record;
static bool s_running;
static size_t s_baseline_internal;

static void release_trace_buffer(void)
{
    /* IDF 6.0.3 rejects the documented NULL/0 detach operation. Point the
     * stopped tracer at a permanent one-record buffer instead. A start/stop
     * also resets its linked list, which otherwise still links old records.
     * No allocation contents are read during this short reset. */
    if (heap_trace_init_standalone(&s_idle_record, 1) == ESP_OK &&
        heap_trace_start(HEAP_TRACE_LEAKS) == ESP_OK &&
        heap_trace_stop() == ESP_OK) {
        heap_caps_free(s_records);
        s_records = NULL;
    } else {
        ESP_LOGW(TAG, "Could not detach trace buffer; keeping it for the next run");
    }
}

esp_err_t voice_heap_diag_start(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_baseline_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (s_records == NULL) {
        s_records = heap_caps_calloc(VOICE_HEAP_TRACE_RECORDS, sizeof(*s_records),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_records == NULL) {
            ESP_LOGE(TAG, "Trace buffer allocation failed (%u bytes), internal largest block=%u",
                     (unsigned)(VOICE_HEAP_TRACE_RECORDS * sizeof(*s_records)),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = heap_trace_init_standalone(s_records, VOICE_HEAP_TRACE_RECORDS);
    if (err != ESP_OK) {
        release_trace_buffer();
        return err;
    }
    /* Print before starting, so the logger's own lazy allocations do not
     * enter the experiment. The record buffer itself is also excluded. */
    ESP_LOGI(TAG, "HEAPTRACE start: records=%u depth=%u buffer=%u baseline_internal=%u",
             (unsigned)VOICE_HEAP_TRACE_RECORDS, (unsigned)CONFIG_HEAP_TRACING_STACK_DEPTH,
             (unsigned)(VOICE_HEAP_TRACE_RECORDS * sizeof(*s_records)),
             (unsigned)s_baseline_internal);
    err = heap_trace_start(HEAP_TRACE_LEAKS);
    s_running = err == ESP_OK;
    if (err != ESP_OK) {
        release_trace_buffer();
    }
    return err;
}

esp_err_t voice_heap_diag_dump(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = heap_trace_stop();
    if (err != ESP_OK) {
        return err;
    }
    s_running = false;
    heap_trace_summary_t summary = { 0 };
    esp_err_t summary_err = heap_trace_summary(&summary);
    ESP_LOGI(TAG, "HEAPTRACE begin: live internal allocations; retain the matching firmware ELF");
    size_t internal_bytes = 0;
    size_t internal_count = 0;
    size_t count = heap_trace_get_count();
    for (size_t index = 0; index < count; ++index) {
        heap_trace_record_t record;
        if (heap_trace_get(index, &record) != ESP_OK || record.address == NULL ||
            record.freed || !esp_ptr_internal(record.address)) {
            continue;
        }
        char callers[CONFIG_HEAP_TRACING_STACK_DEPTH * 12 + 1] = { 0 };
        size_t used = 0;
        for (size_t frame = 0; frame < CONFIG_HEAP_TRACING_STACK_DEPTH && record.alloced_by[frame]; ++frame) {
            int length = snprintf(callers + used, sizeof(callers) - used, "%s%p",
                                   frame == 0 ? "" : ":", record.alloced_by[frame]);
            if (length < 0 || (size_t)length >= sizeof(callers) - used) break;
            used += (size_t)length;
        }
        internal_bytes += record.size;
        ++internal_count;
        ESP_LOGI(TAG, "HEAPTRACE allocation bytes=%u address=%p callers=%s",
                 (unsigned)record.size, record.address, callers);
    }
    ESP_LOGI(TAG, "HEAPTRACE internal records=%u bytes=%u", (unsigned)internal_count, (unsigned)internal_bytes);
    if (summary_err == ESP_OK) {
        ESP_LOGI(TAG, "HEAPTRACE summary: retained=%u high_water=%u capacity=%u overflow=%u allocations=%u frees=%u",
                 (unsigned)summary.count, (unsigned)summary.high_water_mark,
                 (unsigned)summary.capacity, (unsigned)summary.has_overflowed,
                 (unsigned)summary.total_allocations, (unsigned)summary.total_frees);
        if (summary.has_overflowed) {
            ESP_LOGW(TAG, "Trace overflowed: this dump can miss allocations; shorten the experiment or increase the buffer");
        }
    }
    release_trace_buffer();
    ESP_LOGI(TAG, "HEAPTRACE end: baseline_internal=%u current_internal=%u largest_internal=%u",
             (unsigned)s_baseline_internal,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return summary_err;
}

#else

esp_err_t voice_heap_diag_start(void)
{
    ESP_LOGW(TAG, "Heap trace disabled: build with CONFIG_HEAP_TRACING_STANDALONE=y and CONFIG_HEAP_TRACING_STACK_DEPTH=4");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_heap_diag_dump(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
