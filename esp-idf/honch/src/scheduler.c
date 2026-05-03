// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "scheduler.h"
#include "queue.h"
#include "encoder.h"
#include "transport.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "honch";

#define MAX_BATCH_SIZE 50
#define MAX_BACKOFF_MS (5 * 60 * 1000)  // 5 minutes
#define INITIAL_BACKOFF_MS 1000

extern const char *g_honch_api_key;
extern volatile bool g_honch_connected;

static TaskHandle_t s_worker_task = NULL;
static volatile bool s_running = false;
static uint32_t s_flush_interval_ms = 60000;
static uint32_t s_flush_threshold = 30;
static uint32_t s_backoff_ms = INITIAL_BACKOFF_MS;

// Synchronous flush signaling
static volatile bool s_flush_sync_requested = false;
static SemaphoreHandle_t s_flush_sync_done = NULL;

static uint32_t jittered_backoff(uint32_t base_ms)
{
    // +-25% jitter
    uint32_t jitter_range = base_ms / 2;  // 50% range centered on base
    uint32_t jitter = esp_random() % (jitter_range + 1);
    return base_ms - (jitter_range / 2) + jitter;
}

static void do_flush(void)
{
    size_t depth = honch_queue_depth();
    if (depth == 0) {
        return;
    }

    if (!g_honch_connected) {
#ifdef CONFIG_HONCH_LOG_VERBOSE
        ESP_LOGI(TAG, "Not connected, skipping flush");
#endif
        return;
    }

    char *events[MAX_BATCH_SIZE];
    int count = honch_queue_pop(events, MAX_BATCH_SIZE);
    if (count == 0) {
        return;
    }

    ESP_LOGI(TAG, "Flushing %d events", count);

    char *batch_json = honch_encode_batch(g_honch_api_key, events, count);
    if (!batch_json) {
        ESP_LOGE(TAG, "Failed to encode batch");
        honch_queue_requeue(events, count);
        return;
    }

    honch_transport_result_t result = honch_transport_send(batch_json, strlen(batch_json));
    free(batch_json);

    switch (result) {
        case HONCH_TRANSPORT_OK:
            honch_queue_confirm(count);
            for (int i = 0; i < count; i++) {
                free(events[i]);
            }
            s_backoff_ms = INITIAL_BACKOFF_MS;
            break;

        case HONCH_TRANSPORT_AUTH_ERROR:
            ESP_LOGE(TAG, "API key rejected, dropping %d events", count);
            honch_queue_confirm(count);
            for (int i = 0; i < count; i++) {
                free(events[i]);
            }
            break;

        case HONCH_TRANSPORT_SERVER_ERROR:
        case HONCH_TRANSPORT_NETWORK_ERROR:
            ESP_LOGW(TAG, "Flush failed, will retry (backoff: %u ms)", (unsigned)s_backoff_ms);
            honch_queue_requeue(events, count);
            s_backoff_ms = s_backoff_ms * 2;
            if (s_backoff_ms > MAX_BACKOFF_MS) {
                s_backoff_ms = MAX_BACKOFF_MS;
            }
            break;
    }
}

static void worker_task(void *arg)
{
    (void)arg;

    while (s_running) {
        // Wait for notification or timeout
        uint32_t wait_ms = s_flush_interval_ms;

        // If we're in backoff, use that instead
        if (s_backoff_ms > INITIAL_BACKOFF_MS) {
            wait_ms = jittered_backoff(s_backoff_ms);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));

        if (!s_running) {
            break;
        }

        do_flush();

        // Handle sync flush request
        if (s_flush_sync_requested) {
            s_flush_sync_requested = false;
            if (s_flush_sync_done) {
                xSemaphoreGive(s_flush_sync_done);
            }
        }
    }

    vTaskDelete(NULL);
}

honch_err_t honch_scheduler_init(uint32_t flush_interval_seconds,
                                  uint32_t flush_event_threshold)
{
    s_flush_interval_ms = flush_interval_seconds * 1000;
    s_flush_threshold = flush_event_threshold;
    s_backoff_ms = INITIAL_BACKOFF_MS;
    s_running = true;
    s_flush_sync_requested = false;

    s_flush_sync_done = xSemaphoreCreateBinary();
    if (!s_flush_sync_done) {
        return HONCH_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(worker_task, "honch_worker", 8192, NULL, 5, &s_worker_task);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_flush_sync_done);
        s_flush_sync_done = NULL;
        return HONCH_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Scheduler started (interval: %us, threshold: %u)",
             (unsigned)flush_interval_seconds, (unsigned)flush_event_threshold);
    return HONCH_OK;
}

void honch_scheduler_deinit(void)
{
    s_running = false;
    if (s_worker_task) {
        xTaskNotifyGive(s_worker_task);
        // Give the task time to exit
        vTaskDelay(pdMS_TO_TICKS(100));
        s_worker_task = NULL;
    }
    if (s_flush_sync_done) {
        vSemaphoreDelete(s_flush_sync_done);
        s_flush_sync_done = NULL;
    }
}

void honch_scheduler_notify(void)
{
    if (s_worker_task) {
        xTaskNotifyGive(s_worker_task);
    }
}

honch_err_t honch_scheduler_flush_sync(uint32_t timeout_ms)
{
    if (!s_worker_task || !s_flush_sync_done) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    s_flush_sync_requested = true;
    xTaskNotifyGive(s_worker_task);

    if (xSemaphoreTake(s_flush_sync_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Synchronous flush timed out after %u ms", (unsigned)timeout_ms);
        return HONCH_ERR_TIMEOUT;
    }

    return HONCH_OK;
}
