// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if HONCH_ENABLE_CRASH_SYMBOLICATION && \
    defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && \
    defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF) && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#define HONCH_ESP_HAS_COREDUMP_SUMMARY 1
#include "esp_core_dump.h"
#endif

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

#if HONCH_ENABLE_ERROR_TRACKING
typedef struct honch_esp_crash_summary {
    char fault_pc[HONCH_ESP_FAULT_PC_MAX_BYTES + 1u];
    char backtrace[HONCH_ESP_BACKTRACE_MAX_BYTES + 1u];
    char task_name[HONCH_ESP_TASK_NAME_MAX_BYTES + 1u];
} honch_esp_crash_summary_t;

static const char *honch_esp_firmware_build_id(void)
{
    static char build_id[HONCH_ESP_BUILD_ID_MAX_BYTES + 1u];
    int written = esp_app_get_elf_sha256(build_id, sizeof(build_id));
    if (written <= 1 || build_id[0] == '\0') {
        build_id[0] = '\0';
        return NULL;
    }
    return build_id;
}

static bool honch_esp_crash_summary_fill(honch_esp_crash_summary_t *summary)
{
    if (summary == NULL) {
        return false;
    }
    *summary = (honch_esp_crash_summary_t){0};

#if HONCH_ESP_HAS_COREDUMP_SUMMARY
    esp_core_dump_summary_t core_summary = {0};
    if (esp_core_dump_get_summary(&core_summary) != ESP_OK) {
        return false;
    }

    if (core_summary.exc_pc != 0u) {
        (void)snprintf(
            summary->fault_pc,
            sizeof(summary->fault_pc),
            "0x%08lx",
            (unsigned long)core_summary.exc_pc);
    }

#if defined(CONFIG_IDF_TARGET_ARCH_XTENSA) && CONFIG_IDF_TARGET_ARCH_XTENSA
    if (!core_summary.exc_bt_info.corrupted) {
        size_t offset = 0u;
        uint32_t depth = core_summary.exc_bt_info.depth;
        if (depth > (uint32_t)(sizeof(core_summary.exc_bt_info.bt) / sizeof(core_summary.exc_bt_info.bt[0]))) {
            depth = (uint32_t)(sizeof(core_summary.exc_bt_info.bt) / sizeof(core_summary.exc_bt_info.bt[0]));
        }
        for (uint32_t i = 0u; i < depth; i++) {
            uint32_t pc = core_summary.exc_bt_info.bt[i];
            if (pc == 0u) {
                continue;
            }
            int written = snprintf(
                summary->backtrace + offset,
                sizeof(summary->backtrace) - offset,
                "%s0x%08lx",
                offset == 0u ? "" : ",",
                (unsigned long)pc);
            if (written < 0 || (size_t)written >= sizeof(summary->backtrace) - offset) {
                summary->backtrace[0] = '\0';
                break;
            }
            offset += (size_t)written;
        }
    }
#endif

    if (core_summary.exc_task[0] != '\0') {
        (void)snprintf(summary->task_name, sizeof(summary->task_name), "%s", core_summary.exc_task);
    }
    return true;
#endif
    return false;
}

static honch_fault_snapshot_t honch_esp_abnormal_fault_snapshot(
    honch_fault_kind_t kind,
    const char *reset_reason,
    bool include_symbolication_context)
{
    static honch_esp_crash_summary_t summary;
    summary = (honch_esp_crash_summary_t){0};
    bool has_crash_summary = include_symbolication_context && honch_esp_crash_summary_fill(&summary);
    const char *build_id = has_crash_summary ? honch_esp_firmware_build_id() : NULL;

    return (honch_fault_snapshot_t) {
        .kind = kind,
        .severity = HONCH_FAULT_SEVERITY_FATAL,
        .reset_reason = reset_reason,
        .crash_summary_version = has_crash_summary ? 1u : 0u,
        .firmware_build_id = build_id,
        .fault_pc = summary.fault_pc[0] != '\0' ? summary.fault_pc : NULL,
        .backtrace = summary.backtrace[0] != '\0' ? summary.backtrace : NULL,
        .task_name = summary.task_name[0] != '\0' ? summary.task_name : NULL
    };
}

honch_fault_snapshot_t honch_esp_fault_snapshot(bool include_symbolication_context)
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
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_PANIC, "panic", include_symbolication_context);
        case ESP_RST_INT_WDT:
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, "interrupt_wdt", false);
        case ESP_RST_TASK_WDT:
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, "task_wdt", false);
        case ESP_RST_WDT:
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, "watchdog", false);
        case ESP_RST_BROWNOUT:
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_BROWNOUT, "brownout", false);
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
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        case ESP_RST_USB:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "usb"
            };
        case ESP_RST_JTAG:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "jtag"
            };
        case ESP_RST_EFUSE:
            return (honch_fault_snapshot_t) {
                .kind = HONCH_FAULT_KIND_NONE,
                .severity = HONCH_FAULT_SEVERITY_INFO,
                .reset_reason = "efuse"
            };
        case ESP_RST_PWR_GLITCH:
            return honch_esp_abnormal_fault_snapshot(
                HONCH_FAULT_KIND_BROWNOUT,
                "power_glitch",
                false);
#endif
        case ESP_RST_UNKNOWN:
        default:
            return honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_UNKNOWN, "unknown", false);
    }
}
#endif
