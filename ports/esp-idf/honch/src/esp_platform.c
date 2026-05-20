// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stddef.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "honch";
static const int64_t HONCH_MIN_UNIX_TIME_SECONDS = 1577836800;

static uint64_t honch_esp_now_ms(void *ctx)
{
    (void)ctx;
    struct timeval now;
    if (gettimeofday(&now, NULL) == 0 && now.tv_sec >= HONCH_MIN_UNIX_TIME_SECONDS) {
        return ((uint64_t)now.tv_sec * 1000u) + ((uint64_t)now.tv_usec / 1000u);
    }

    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t honch_esp_uptime_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static honch_status_t honch_esp_random_bytes(void *ctx, uint8_t *buffer, size_t buffer_size)
{
    (void)ctx;
    if (buffer_size == 0u) {
        return HONCH_STATUS_OK;
    }
    if (buffer == NULL && buffer_size > 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    esp_fill_random(buffer, buffer_size);
    return HONCH_STATUS_OK;
}

static honch_status_t honch_esp_lock(void *ctx)
{
    honch_esp_platform_t *platform = (honch_esp_platform_t *)ctx;
    if (platform == NULL || platform->mutex == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return xSemaphoreTake(platform->mutex, portMAX_DELAY) == pdTRUE ? HONCH_STATUS_OK : HONCH_STATUS_ERROR_IO;
}

static honch_status_t honch_esp_unlock(void *ctx)
{
    honch_esp_platform_t *platform = (honch_esp_platform_t *)ctx;
    if (platform == NULL || platform->mutex == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return xSemaphoreGive(platform->mutex) == pdTRUE ? HONCH_STATUS_OK : HONCH_STATUS_ERROR_IO;
}

static void honch_esp_log(void *ctx, honch_log_level_t level, const char *message)
{
    (void)ctx;
    const char *text = message == NULL ? "" : message;
    switch (level) {
        case HONCH_LOG_DEBUG:
            ESP_LOGD(TAG, "%s", text);
            break;
        case HONCH_LOG_INFO:
            ESP_LOGI(TAG, "%s", text);
            break;
        case HONCH_LOG_WARN:
            ESP_LOGW(TAG, "%s", text);
            break;
        case HONCH_LOG_ERROR:
        default:
            ESP_LOGE(TAG, "%s", text);
            break;
    }
}

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    ctx->mutex = NULL;
    ctx->mutex = xSemaphoreCreateMutex();
    if (ctx->mutex == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }

    *ops = (honch_platform_ops_t) {
        .now_ms = honch_esp_now_ms,
        .uptime_ms = honch_esp_uptime_ms,
        .random_bytes = honch_esp_random_bytes,
        .lock = honch_esp_lock,
        .unlock = honch_esp_unlock,
        .log = honch_esp_log,
        .ctx = ctx
    };
    return HONCH_STATUS_OK;
}

void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx)
{
    if (ctx == NULL || ctx->mutex == NULL) {
        return;
    }

    vSemaphoreDelete(ctx->mutex);
    ctx->mutex = NULL;
}
