// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "queue.h"
#include "perf.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_HONCH_MAX_QUEUE_DEPTH
#define MAX_QUEUE_DEPTH CONFIG_HONCH_MAX_QUEUE_DEPTH
#else
#define MAX_QUEUE_DEPTH 256
#endif

static const char *TAG = "honch";
static const char *NVS_NAMESPACE = "honch_q";

static SemaphoreHandle_t s_mutex = NULL;
static uint32_t s_head = 0;  // next write position
static uint32_t s_tail = 0;  // next read position

static void load_counters(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, "head", &s_head);
        nvs_get_u32(handle, "tail", &s_tail);
        nvs_close(handle);
    }
}

static void save_counters(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, "head", s_head);
        nvs_set_u32(handle, "tail", s_tail);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void erase_key(uint32_t index)
{
    char key[16];
    snprintf(key, sizeof(key), "e_%u", (unsigned)index);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, key);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

honch_err_t honch_queue_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return HONCH_ERR_NO_MEM;
    }

    s_head = 0;
    s_tail = 0;
    load_counters();

    ESP_LOGI(TAG, "Queue initialized, depth: %u", (unsigned)(s_head - s_tail));
    return HONCH_OK;
}

void honch_queue_deinit(void)
{
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

honch_err_t honch_queue_push(honch_payload_t *payload)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!payload || !payload->data || payload->len == 0) {
        return HONCH_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        honch_payload_free(payload);
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    while ((s_head - s_tail) >= MAX_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "Queue full, dropping oldest event (idx %u)", (unsigned)s_tail);
        erase_key(s_tail);
        s_tail++;
    }

    char key[16];
    snprintf(key, sizeof(key), "e_%u", (unsigned)s_head);

    nvs_handle_t handle;
    int64_t open_start_us = HONCH_PERF_NOW_US();
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    int64_t open_us = HONCH_PERF_ELAPSED_US(open_start_us);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        honch_payload_free(payload);
        HONCH_PERF_LOG("HONCH_PERF_QUEUE_PUSH",
                       "result=nvs_open_error code=%d total_us=%" PRId64
                       " nvs_open_us=%" PRId64,
                       err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       open_us);
        return HONCH_ERR_NVS;
    }

    int64_t set_blob_start_us = HONCH_PERF_NOW_US();
    err = nvs_set_blob(handle, key, payload->data, payload->len);
    int64_t set_blob_us = HONCH_PERF_ELAPSED_US(set_blob_start_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write event to NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        xSemaphoreGive(s_mutex);
        honch_payload_free(payload);
        HONCH_PERF_LOG("HONCH_PERF_QUEUE_PUSH",
                       "result=nvs_set_blob_error code=%d total_us=%" PRId64
                       " nvs_open_us=%" PRId64 " nvs_set_blob_us=%" PRId64,
                       err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       open_us,
                       set_blob_us);
        return HONCH_ERR_NVS;
    }

    s_head++;
    int64_t counters_start_us = HONCH_PERF_NOW_US();
    nvs_set_u32(handle, "head", s_head);
    nvs_set_u32(handle, "tail", s_tail);
    int64_t counters_us = HONCH_PERF_ELAPSED_US(counters_start_us);
    int64_t commit_start_us = HONCH_PERF_NOW_US();
    nvs_commit(handle);
    int64_t commit_us = HONCH_PERF_ELAPSED_US(commit_start_us);
    nvs_close(handle);

    size_t payload_len = payload->len;
    honch_payload_free(payload);

#ifdef CONFIG_HONCH_LOG_VERBOSE
    ESP_LOGI(TAG, "Event queued (depth: %u)", (unsigned)(s_head - s_tail));
#endif

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_PUSH",
                   "result=ok total_us=%" PRId64 " nvs_open_us=%" PRId64
                   " nvs_set_blob_us=%" PRId64 " nvs_counters_us=%" PRId64
                   " nvs_commit_us=%" PRId64 " payload_bytes=%u depth=%u"
                   " heap_before=%" PRIu32 " heap_after=%" PRIu32,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   open_us,
                   set_blob_us,
                   counters_us,
                   commit_us,
                   (unsigned)payload_len,
                   (unsigned)(s_head - s_tail),
                   heap_before,
                   HONCH_PERF_HEAP_FREE());

    xSemaphoreGive(s_mutex);
    return HONCH_OK;
}

int honch_queue_pop(honch_payload_t *events_out, int max_events)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!s_mutex || !events_out) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int count = 0;
    size_t total_bytes = 0;
    uint32_t idx = s_tail;
    bool counters_changed = false;

    while (idx < s_head && count < max_events) {
        char key[16];
        snprintf(key, sizeof(key), "e_%u", (unsigned)idx);

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            break;
        }

        size_t len = 0;
        esp_err_t err = nvs_get_blob(handle, key, NULL, &len);
        if (err != ESP_OK || len == 0) {
            nvs_close(handle);
            erase_key(idx);
            if (idx == s_tail) {
                s_tail++;
                counters_changed = true;
            }
            idx++;
            continue;
        }

        uint8_t *buf = malloc(len);
        if (!buf) {
            nvs_close(handle);
            break;
        }

        err = nvs_get_blob(handle, key, buf, &len);
        nvs_close(handle);

        if (err != ESP_OK) {
            free(buf);
            erase_key(idx);
            if (idx == s_tail) {
                s_tail++;
                counters_changed = true;
            }
            idx++;
            continue;
        }

        events_out[count].data = buf;
        events_out[count].len = len;
        total_bytes += len;
        count++;
        idx++;
    }

    if (counters_changed) {
        save_counters();
    }

    xSemaphoreGive(s_mutex);
    HONCH_PERF_LOG("HONCH_PERF_QUEUE_POP",
                   "count=%d bytes=%u total_us=%" PRId64 " max_events=%d"
                   " depth_before=%u depth_after=%u heap_before=%" PRIu32
                   " heap_after=%" PRIu32,
                   count,
                   (unsigned)total_bytes,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   max_events,
                   (unsigned)(s_head - s_tail + count),
                   (unsigned)(s_head - s_tail),
                   heap_before,
                   HONCH_PERF_HEAP_FREE());
    return count;
}

honch_err_t honch_queue_confirm(int count)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();

    if (!s_mutex) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nvs_handle_t handle;
    int64_t open_start_us = HONCH_PERF_NOW_US();
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    int64_t open_us = HONCH_PERF_ELAPSED_US(open_start_us);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        HONCH_PERF_LOG("HONCH_PERF_QUEUE_CONFIRM",
                       "result=nvs_open_error code=%d total_us=%" PRId64
                       " nvs_open_us=%" PRId64,
                       err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       open_us);
        return HONCH_ERR_NVS;
    }

    int erased = 0;
    int64_t erase_start_us = HONCH_PERF_NOW_US();
    for (int i = 0; i < count && s_tail < s_head; i++) {
        char key[16];
        snprintf(key, sizeof(key), "e_%u", (unsigned)s_tail);
        nvs_erase_key(handle, key);
        s_tail++;
        erased++;
    }
    int64_t erase_us = HONCH_PERF_ELAPSED_US(erase_start_us);

    nvs_set_u32(handle, "head", s_head);
    nvs_set_u32(handle, "tail", s_tail);
    int64_t commit_start_us = HONCH_PERF_NOW_US();
    nvs_commit(handle);
    int64_t commit_us = HONCH_PERF_ELAPSED_US(commit_start_us);
    nvs_close(handle);

    xSemaphoreGive(s_mutex);
    HONCH_PERF_LOG("HONCH_PERF_QUEUE_CONFIRM",
                   "result=ok requested=%d erased=%d total_us=%" PRId64
                   " nvs_open_us=%" PRId64 " nvs_erase_us=%" PRId64
                   " nvs_commit_us=%" PRId64 " depth=%u",
                   count,
                   erased,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   open_us,
                   erase_us,
                   commit_us,
                   (unsigned)(s_head - s_tail));
    return HONCH_OK;
}

honch_err_t honch_queue_requeue(honch_payload_t *events, int count)
{
    // Events are still in NVS since we haven't confirmed them.
    // Just free the memory; they'll be popped again next time.
    for (int i = 0; i < count; i++) {
        honch_payload_free(&events[i]);
    }
    return HONCH_OK;
}

size_t honch_queue_depth(void)
{
    if (!s_mutex) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t depth = s_head - s_tail;
    xSemaphoreGive(s_mutex);
    return depth;
}

honch_err_t honch_queue_clear(void)
{
    if (!s_mutex) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    s_head = 0;
    s_tail = 0;

    xSemaphoreGive(s_mutex);
    return HONCH_OK;
}
