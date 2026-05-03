// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

honch_err_t honch_queue_push(char *event_json)
{
    if (!event_json) {
        return HONCH_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        free(event_json);
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Check if queue is full, drop oldest if so
    while ((s_head - s_tail) >= MAX_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "Queue full, dropping oldest event (idx %u)", (unsigned)s_tail);
        char key[16];
        snprintf(key, sizeof(key), "e_%u", (unsigned)s_tail);

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, key);
            nvs_commit(handle);
            nvs_close(handle);
        }
        s_tail++;
    }

    // Store event in NVS
    char key[16];
    snprintf(key, sizeof(key), "e_%u", (unsigned)s_head);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        free(event_json);
        return HONCH_ERR_NVS;
    }

    err = nvs_set_str(handle, key, event_json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write event to NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        xSemaphoreGive(s_mutex);
        free(event_json);
        return HONCH_ERR_NVS;
    }

    s_head++;
    nvs_set_u32(handle, "head", s_head);
    nvs_set_u32(handle, "tail", s_tail);
    nvs_commit(handle);
    nvs_close(handle);

    free(event_json);

#ifdef CONFIG_HONCH_LOG_VERBOSE
    ESP_LOGI(TAG, "Event queued (depth: %u)", (unsigned)(s_head - s_tail));
#endif

    xSemaphoreGive(s_mutex);
    return HONCH_OK;
}

int honch_queue_pop(char **events_out, int max_events)
{
    if (!s_mutex || !events_out) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int count = 0;
    uint32_t idx = s_tail;

    while (idx < s_head && count < max_events) {
        char key[16];
        snprintf(key, sizeof(key), "e_%u", (unsigned)idx);

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            break;
        }

        size_t len = 0;
        esp_err_t err = nvs_get_str(handle, key, NULL, &len);
        if (err != ESP_OK || len == 0) {
            nvs_close(handle);
            idx++;
            continue;
        }

        char *buf = malloc(len);
        if (!buf) {
            nvs_close(handle);
            break;
        }

        err = nvs_get_str(handle, key, buf, &len);
        nvs_close(handle);

        if (err != ESP_OK) {
            free(buf);
            idx++;
            continue;
        }

        events_out[count++] = buf;
        idx++;
    }

    xSemaphoreGive(s_mutex);
    return count;
}

honch_err_t honch_queue_confirm(int count)
{
    if (!s_mutex) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return HONCH_ERR_NVS;
    }

    for (int i = 0; i < count && s_tail < s_head; i++) {
        char key[16];
        snprintf(key, sizeof(key), "e_%u", (unsigned)s_tail);
        nvs_erase_key(handle, key);
        s_tail++;
    }

    nvs_set_u32(handle, "head", s_head);
    nvs_set_u32(handle, "tail", s_tail);
    nvs_commit(handle);
    nvs_close(handle);

    xSemaphoreGive(s_mutex);
    return HONCH_OK;
}

honch_err_t honch_queue_requeue(char **events, int count)
{
    // Events are still in NVS since we haven't confirmed them.
    // Just free the memory; they'll be popped again next time.
    for (int i = 0; i < count; i++) {
        free(events[i]);
        events[i] = NULL;
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
