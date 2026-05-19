// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "queue.h"
#include "perf.h"

#include <inttypes.h>
#include <stdbool.h>
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

#ifdef CONFIG_HONCH_RAM_QUEUE_DEPTH
#define RAM_QUEUE_DEPTH CONFIG_HONCH_RAM_QUEUE_DEPTH
#else
#define RAM_QUEUE_DEPTH 64
#endif

static const char *TAG = "honch";
static const char *NVS_NAMESPACE = "honch_q";

static SemaphoreHandle_t s_mutex = NULL;

static honch_payload_t *s_ram_events = NULL;
static uint32_t s_ram_head = 0;  // next write position
static uint32_t s_ram_tail = 0;  // next read position
static uint32_t s_ram_count = 0;

static uint32_t s_nvs_head = 0;  // next write position
static uint32_t s_nvs_tail = 0;  // next read position

static bool s_last_pop_valid = false;
static int s_last_pop_count = 0;
static int s_last_pop_nvs_count = 0;
static int s_last_pop_ram_count = 0;
static uint32_t s_last_pop_nvs_start = 0;

static size_t nvs_depth(void)
{
    return s_nvs_head - s_nvs_tail;
}

static size_t total_depth(void)
{
    return nvs_depth() + s_ram_count;
}

static void make_key(uint32_t index, char *key, size_t key_size)
{
    snprintf(key, key_size, "e_%u", (unsigned)index);
}

static void load_counters(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, "head", &s_nvs_head);
        nvs_get_u32(handle, "tail", &s_nvs_tail);
        nvs_close(handle);
    }
}

static void save_counters(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, "head", s_nvs_head);
        nvs_set_u32(handle, "tail", s_nvs_tail);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void erase_key(uint32_t index)
{
    char key[16];
    make_key(index, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, key);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void reset_last_pop(void)
{
    s_last_pop_valid = false;
    s_last_pop_count = 0;
    s_last_pop_nvs_count = 0;
    s_last_pop_ram_count = 0;
    s_last_pop_nvs_start = 0;
}

static void drop_oldest_locked(void)
{
    if (nvs_depth() > 0) {
        ESP_LOGW(TAG, "Queue full, dropping oldest NVS event (idx %u)", (unsigned)s_nvs_tail);
        erase_key(s_nvs_tail);
        s_nvs_tail++;
        save_counters();
        return;
    }

    if (s_ram_count > 0) {
        honch_payload_t dropped = s_ram_events[s_ram_tail];
        memset(&s_ram_events[s_ram_tail], 0, sizeof(s_ram_events[s_ram_tail]));
        s_ram_tail = (s_ram_tail + 1) % RAM_QUEUE_DEPTH;
        s_ram_count--;
        ESP_LOGW(TAG, "Queue full, dropping oldest RAM event");
        honch_payload_free(&dropped);
    }
}

static honch_err_t ram_enqueue(honch_payload_t *payload)
{
    if (s_ram_count >= RAM_QUEUE_DEPTH) {
        return HONCH_ERR_QUEUE_FULL;
    }

    s_ram_events[s_ram_head] = *payload;
    s_ram_head = (s_ram_head + 1) % RAM_QUEUE_DEPTH;
    s_ram_count++;

    payload->data = NULL;
    payload->len = 0;
    return HONCH_OK;
}

static bool ram_pop(honch_payload_t *payload)
{
    if (s_ram_count == 0) {
        return false;
    }

    *payload = s_ram_events[s_ram_tail];
    memset(&s_ram_events[s_ram_tail], 0, sizeof(s_ram_events[s_ram_tail]));
    s_ram_tail = (s_ram_tail + 1) % RAM_QUEUE_DEPTH;
    s_ram_count--;
    return true;
}

static honch_err_t nvs_append_payload(const uint8_t *data, size_t len, const char *reason)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!data || len == 0) {
        return HONCH_ERR_INVALID_ARG;
    }

    while (nvs_depth() >= MAX_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "NVS queue full, dropping oldest event (idx %u)", (unsigned)s_nvs_tail);
        erase_key(s_nvs_tail);
        s_nvs_tail++;
    }

    char key[16];
    make_key(s_nvs_head, key, sizeof(key));

    nvs_handle_t handle;
    int64_t open_start_us = HONCH_PERF_NOW_US();
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    int64_t open_us = HONCH_PERF_ELAPSED_US(open_start_us);
    if (err != ESP_OK) {
        HONCH_PERF_LOG("HONCH_PERF_NVS_SPILL",
                       "result=nvs_open_error reason=%s code=%d total_us=%" PRId64
                       " nvs_open_us=%" PRId64,
                       reason,
                       err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       open_us);
        return HONCH_ERR_NVS;
    }

    int64_t set_blob_start_us = HONCH_PERF_NOW_US();
    err = nvs_set_blob(handle, key, data, len);
    int64_t set_blob_us = HONCH_PERF_ELAPSED_US(set_blob_start_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write event to NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        HONCH_PERF_LOG("HONCH_PERF_NVS_SPILL",
                       "result=nvs_set_blob_error reason=%s code=%d total_us=%" PRId64
                       " nvs_open_us=%" PRId64 " nvs_set_blob_us=%" PRId64,
                       reason,
                       err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       open_us,
                       set_blob_us);
        return HONCH_ERR_NVS;
    }

    s_nvs_head++;
    int64_t counters_start_us = HONCH_PERF_NOW_US();
    nvs_set_u32(handle, "head", s_nvs_head);
    nvs_set_u32(handle, "tail", s_nvs_tail);
    int64_t counters_us = HONCH_PERF_ELAPSED_US(counters_start_us);
    int64_t commit_start_us = HONCH_PERF_NOW_US();
    nvs_commit(handle);
    int64_t commit_us = HONCH_PERF_ELAPSED_US(commit_start_us);
    nvs_close(handle);

    HONCH_PERF_LOG("HONCH_PERF_NVS_SPILL",
                   "result=ok reason=%s total_us=%" PRId64 " nvs_open_us=%" PRId64
                   " nvs_set_blob_us=%" PRId64 " nvs_counters_us=%" PRId64
                   " nvs_commit_us=%" PRId64 " payload_bytes=%u nvs_depth=%u"
                   " ram_depth=%u heap_before=%" PRIu32 " heap_after=%" PRIu32,
                   reason,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   open_us,
                   set_blob_us,
                   counters_us,
                   commit_us,
                   (unsigned)len,
                   (unsigned)nvs_depth(),
                   (unsigned)s_ram_count,
                   heap_before,
                   HONCH_PERF_HEAP_FREE());
    return HONCH_OK;
}

honch_err_t honch_queue_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return HONCH_ERR_NO_MEM;
    }

    s_ram_events = calloc(RAM_QUEUE_DEPTH, sizeof(honch_payload_t));
    if (!s_ram_events) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return HONCH_ERR_NO_MEM;
    }

    s_ram_head = 0;
    s_ram_tail = 0;
    s_ram_count = 0;
    s_nvs_head = 0;
    s_nvs_tail = 0;
    reset_last_pop();
    load_counters();

    ESP_LOGI(TAG,
             "Queue initialized, depth: %u (ram: %u, nvs: %u)",
             (unsigned)total_depth(),
             (unsigned)s_ram_count,
             (unsigned)nvs_depth());
    return HONCH_OK;
}

void honch_queue_deinit(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    if (s_ram_events) {
        for (uint32_t i = 0; i < RAM_QUEUE_DEPTH; i++) {
            honch_payload_free(&s_ram_events[i]);
        }
        free(s_ram_events);
        s_ram_events = NULL;
    }

    s_ram_head = 0;
    s_ram_tail = 0;
    s_ram_count = 0;
    reset_last_pop();

    if (s_mutex) {
        xSemaphoreGive(s_mutex);
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
    if (!s_mutex || !s_ram_events) {
        honch_payload_free(payload);
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    while (total_depth() >= MAX_QUEUE_DEPTH) {
        drop_oldest_locked();
    }

#ifdef CONFIG_HONCH_DURABLE_IMMEDIATE_NVS
    honch_err_t err = nvs_append_payload(payload->data, payload->len, "immediate");
    size_t payload_len = payload->len;
    honch_payload_free(payload);
    xSemaphoreGive(s_mutex);

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_PUSH",
                   "result=%s backend=nvs total_us=%" PRId64 " payload_bytes=%u"
                   " depth=%u heap_before=%" PRIu32 " heap_after=%" PRIu32,
                   err == HONCH_OK ? "ok" : "error",
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   (unsigned)payload_len,
                   (unsigned)honch_queue_depth(),
                   heap_before,
                   HONCH_PERF_HEAP_FREE());
    return err;
#else
    honch_err_t err = ram_enqueue(payload);
    if (err == HONCH_OK) {
#ifdef CONFIG_HONCH_LOG_VERBOSE
        ESP_LOGI(TAG, "Event queued in RAM (depth: %u)", (unsigned)total_depth());
#endif
        HONCH_PERF_LOG("HONCH_PERF_RAM_QUEUE_PUSH",
                       "result=ok total_us=%" PRId64 " ram_depth=%u nvs_depth=%u"
                       " total_depth=%u heap_before=%" PRIu32 " heap_after=%" PRIu32,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       (unsigned)s_ram_count,
                       (unsigned)nvs_depth(),
                       (unsigned)total_depth(),
                       heap_before,
                       HONCH_PERF_HEAP_FREE());
        xSemaphoreGive(s_mutex);
        return HONCH_OK;
    }

    size_t payload_len = payload->len;
    err = nvs_append_payload(payload->data, payload->len, "ram_full");
    honch_payload_free(payload);
    xSemaphoreGive(s_mutex);

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_PUSH",
                   "result=%s backend=nvs_spill total_us=%" PRId64
                   " payload_bytes=%u depth=%u heap_before=%" PRIu32
                   " heap_after=%" PRIu32,
                   err == HONCH_OK ? "ok" : "error",
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   (unsigned)payload_len,
                   (unsigned)honch_queue_depth(),
                   heap_before,
                   HONCH_PERF_HEAP_FREE());
    return err;
#endif
}

int honch_queue_pop(honch_payload_t *events_out, int max_events)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!s_mutex || !events_out || max_events <= 0) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t depth_before = total_depth();
    reset_last_pop();

    int count = 0;
    int nvs_count = 0;
    int ram_count = 0;
    size_t total_bytes = 0;
    uint32_t idx = s_nvs_tail;
    bool counters_changed = false;

    s_last_pop_nvs_start = s_nvs_tail;

    while (idx < s_nvs_head && count < max_events) {
        char key[16];
        make_key(idx, key, sizeof(key));

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            break;
        }

        size_t len = 0;
        esp_err_t err = nvs_get_blob(handle, key, NULL, &len);
        if (err != ESP_OK || len == 0) {
            nvs_close(handle);
            erase_key(idx);
            if (idx == s_nvs_tail) {
                s_nvs_tail++;
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
            if (idx == s_nvs_tail) {
                s_nvs_tail++;
                counters_changed = true;
            }
            idx++;
            continue;
        }

        events_out[count].data = buf;
        events_out[count].len = len;
        total_bytes += len;
        count++;
        nvs_count++;
        idx++;
    }

    while (count < max_events && ram_pop(&events_out[count])) {
        total_bytes += events_out[count].len;
        count++;
        ram_count++;
    }

    if (counters_changed) {
        save_counters();
    }

    s_last_pop_valid = count > 0;
    s_last_pop_count = count;
    s_last_pop_nvs_count = nvs_count;
    s_last_pop_ram_count = ram_count;

    size_t depth_after = total_depth();
    xSemaphoreGive(s_mutex);

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_POP",
                   "count=%d nvs_count=%d ram_count=%d bytes=%u total_us=%" PRId64
                   " max_events=%d depth_before=%u depth_after=%u"
                   " heap_before=%" PRIu32 " heap_after=%" PRIu32,
                   count,
                   nvs_count,
                   ram_count,
                   (unsigned)total_bytes,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   max_events,
                   (unsigned)depth_before,
                   (unsigned)depth_after,
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

    int requested = count;
    if (!s_last_pop_valid || count <= 0) {
        reset_last_pop();
        xSemaphoreGive(s_mutex);
        HONCH_PERF_LOG("HONCH_PERF_QUEUE_CONFIRM",
                       "result=no_last_pop requested=%d total_us=%" PRId64,
                       requested,
                       HONCH_PERF_ELAPSED_US(total_start_us));
        return HONCH_OK;
    }

    if (count > s_last_pop_count) {
        count = s_last_pop_count;
    }

    int nvs_to_erase = s_last_pop_nvs_count;
    if (nvs_to_erase > count) {
        nvs_to_erase = count;
    }
    int ram_confirmed = count - nvs_to_erase;
    if (ram_confirmed > s_last_pop_ram_count) {
        ram_confirmed = s_last_pop_ram_count;
    }

    int64_t open_us = 0;
    int erased = 0;
    int64_t erase_us = 0;
    int64_t commit_us = 0;
    if (nvs_to_erase > 0) {
        nvs_handle_t handle;
        int64_t open_start_us = HONCH_PERF_NOW_US();
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        open_us = HONCH_PERF_ELAPSED_US(open_start_us);
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

        int64_t erase_start_us = HONCH_PERF_NOW_US();
        for (int i = 0; i < nvs_to_erase; i++) {
            uint32_t idx = s_last_pop_nvs_start + (uint32_t)i;
            char key[16];
            make_key(idx, key, sizeof(key));
            nvs_erase_key(handle, key);
            if (s_nvs_tail <= idx) {
                s_nvs_tail = idx + 1;
            }
            erased++;
        }
        erase_us = HONCH_PERF_ELAPSED_US(erase_start_us);

        int64_t commit_start_us = HONCH_PERF_NOW_US();
        nvs_set_u32(handle, "head", s_nvs_head);
        nvs_set_u32(handle, "tail", s_nvs_tail);
        nvs_commit(handle);
        commit_us = HONCH_PERF_ELAPSED_US(commit_start_us);
        nvs_close(handle);
    }

    reset_last_pop();
    size_t depth = total_depth();
    xSemaphoreGive(s_mutex);

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_CONFIRM",
                   "result=ok requested=%d erased=%d ram_confirmed=%d total_us=%" PRId64
                   " nvs_open_us=%" PRId64 " nvs_erase_us=%" PRId64
                   " nvs_commit_us=%" PRId64 " depth=%u",
                   requested,
                   erased,
                   ram_confirmed,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   open_us,
                   erase_us,
                   commit_us,
                   (unsigned)depth);
    return HONCH_OK;
}

honch_err_t honch_queue_requeue(honch_payload_t *events, int count)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();

    if (!events || count <= 0) {
        return HONCH_OK;
    }
    if (!s_mutex) {
        for (int i = 0; i < count; i++) {
            honch_payload_free(&events[i]);
        }
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int ram_spilled = 0;
    int nvs_kept = 0;
    honch_err_t result = HONCH_OK;

    int nvs_count = s_last_pop_valid ? s_last_pop_nvs_count : 0;
    if (nvs_count > count) {
        nvs_count = count;
    }

    for (int i = 0; i < count; i++) {
        if (i < nvs_count) {
            nvs_kept++;
            continue;
        }

        honch_err_t err = nvs_append_payload(events[i].data, events[i].len, "requeue");
        if (err == HONCH_OK) {
            ram_spilled++;
        } else {
            result = err;
        }
    }

    reset_last_pop();
    size_t depth = total_depth();
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < count; i++) {
        honch_payload_free(&events[i]);
    }

    HONCH_PERF_LOG("HONCH_PERF_QUEUE_REQUEUE",
                   "result=%s count=%d nvs_kept=%d ram_spilled=%d total_us=%" PRId64
                   " depth=%u",
                   result == HONCH_OK ? "ok" : "error",
                   count,
                   nvs_kept,
                   ram_spilled,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   (unsigned)depth);
    return result;
}

size_t honch_queue_depth(void)
{
    if (!s_mutex) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t depth = total_depth();
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

    if (s_ram_events) {
        for (uint32_t i = 0; i < RAM_QUEUE_DEPTH; i++) {
            honch_payload_free(&s_ram_events[i]);
        }
    }

    s_ram_head = 0;
    s_ram_tail = 0;
    s_ram_count = 0;
    s_nvs_head = 0;
    s_nvs_tail = 0;
    reset_last_pop();

    xSemaphoreGive(s_mutex);
    return HONCH_OK;
}
