// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "honch";
static const int64_t HONCH_MIN_UNIX_TIME_SECONDS = 1577836800;
#define HONCH_ESP_MUTEX_LOCK_TIMEOUT_MS 10u

static TickType_t honch_esp_lock_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

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

static honch_status_t honch_esp_mutex_create(void *ctx, void **mutex)
{
    (void)ctx;
    if (mutex == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    SemaphoreHandle_t handle = xSemaphoreCreateMutex();
    if (handle == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    *mutex = handle;
    return HONCH_STATUS_OK;
}

static void honch_esp_mutex_destroy(void *ctx, void *mutex)
{
    (void)ctx;
    if (mutex != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t)mutex);
    }
}

static honch_status_t honch_esp_mutex_lock(void *ctx, void *mutex)
{
    (void)ctx;
    if (mutex == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return xSemaphoreTake((SemaphoreHandle_t)mutex, honch_esp_lock_ticks(HONCH_ESP_MUTEX_LOCK_TIMEOUT_MS)) == pdTRUE ?
        HONCH_STATUS_OK :
        HONCH_STATUS_ERROR_BUSY;
}

static void honch_esp_mutex_unlock(void *ctx, void *mutex)
{
    (void)ctx;
    if (mutex != NULL) {
        (void)xSemaphoreGive((SemaphoreHandle_t)mutex);
    }
}

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    ctx->reserved = NULL;
    *ops = (honch_platform_ops_t) {
        .now_ms = honch_esp_now_ms,
        .uptime_ms = honch_esp_uptime_ms,
        .random_bytes = honch_esp_random_bytes,
        .log = honch_esp_log,
        .mutex_create = honch_esp_mutex_create,
        .mutex_destroy = honch_esp_mutex_destroy,
        .mutex_lock = honch_esp_mutex_lock,
        .mutex_unlock = honch_esp_mutex_unlock,
        .ctx = ctx
    };
    return HONCH_STATUS_OK;
}

void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx)
{
    (void)ctx;
}

honch_status_t honch_esp_default_device_id(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return HONCH_STATUS_ERROR_INTERNAL;
    }

    int written = snprintf(
        buffer,
        buffer_size,
        "esp32-%02X%02X%02X%02X%02X%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_STATUS_OK;
}

honch_fault_snapshot_t honch_esp_fault_snapshot(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "power_on"
            };
        case ESP_RST_SW:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "software"
            };
        case ESP_RST_DEEPSLEEP:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "deep_sleep"
            };
        case ESP_RST_PANIC:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_PANIC,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "panic"
            };
        case ESP_RST_INT_WDT:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_WATCHDOG,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "interrupt_wdt"
            };
        case ESP_RST_TASK_WDT:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_WATCHDOG,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "task_wdt"
            };
        case ESP_RST_WDT:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_WATCHDOG,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "watchdog"
            };
        case ESP_RST_BROWNOUT:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_BROWNOUT,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "brownout"
            };
        case ESP_RST_SDIO:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "sdio"
            };
        case ESP_RST_EXT:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "external"
            };
        case ESP_RST_UNKNOWN:
        default:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_UNKNOWN,
                .severity = HONCH_FAULT_SEVERITY_FATAL,
                .reset_reason = "unknown"
            };
    }
}
