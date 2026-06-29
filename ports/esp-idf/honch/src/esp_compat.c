// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#define HONCH_CORE_NO_SHORT_STATUS_NAMES
#include "honch/core/honch.h"

#include "esp_core_adapter.h"
#include "honch_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "honch";
#define HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS 10u

static honch_client_t *s_client = NULL;
static bool s_client_initializing = false;
static bool s_client_shutting_down = false;
static StaticSemaphore_t s_client_mutex_storage;
static SemaphoreHandle_t s_client_mutex = NULL;
static portMUX_TYPE s_client_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static honch_esp_platform_t s_platform_ctx;
static honch_esp_storage_t s_storage_ctx;
static honch_esp_transport_t s_transport_ctx;
#if HONCH_ENABLE_ERROR_TRACKING
static bool s_fault_snapshot_consumed = false;

/* The erase-after-ack callback is only meaningful (and esp_core_dump.h is only
 * on the include path) when a flash ELF coredump is actually configured — the
 * same condition esp_platform.c uses to read the crash summary. */
/* Gate on coredump-to-flash only; ESP_COREDUMP_ENABLE auto-selects the ELF
 * format and CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF is select-only (absent from
 * sdkconfig.h in IDF v6), so requiring it silently compiled out crash capture.
 * See esp_platform.c for the matching summary gate. */
#if HONCH_ENABLE_CRASH_SYMBOLICATION && \
    defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#define HONCH_ESP_COMPAT_HAS_COREDUMP 1
#include "esp_core_dump.h"
#include "esp_partition.h"

/* Erase-after-ack: invoked once the recovered $crash has reached Capture, so the
 * stored coredump is not re-reported on the next boot. NOTE: when a
 * coredump_source is also wired (it always is in this build), the core SUPPRESSES
 * this callback and lets the source's clear() be the single erase — see
 * honch_esp_coredump_clear and config.h. It is kept wired as the fallback for a
 * build that streams no raw blob. */
static void honch_esp_crash_uploaded(void *userdata)
{
    (void)userdata;
    (void)esp_core_dump_image_erase();
}

/* Flash-backed view over the ESP-IDF coredump partition for the SDK's streaming
 * uploader. The image bounds come from esp_core_dump_image_get() (resolved once
 * and cached); bytes are read straight from the coredump data partition so the
 * multi-KB ELF is never materialized in RAM. clear() erases the partition after
 * the blob's final ack (erase-after-ack). */
typedef struct honch_esp_coredump_ctx {
    const esp_partition_t *part; /* the coredump data partition */
    size_t image_offset;         /* image start, relative to the partition */
    size_t image_size;           /* bytes of the stored coredump image */
    bool resolved;               /* image_get has been attempted */
    bool available;              /* a non-empty image is present */
} honch_esp_coredump_ctx_t;

static honch_esp_coredump_ctx_t s_coredump_ctx;
static honch_coredump_source_t s_coredump_source;

static bool honch_esp_coredump_resolve(honch_esp_coredump_ctx_t *ctx)
{
    if (ctx->resolved) {
        return ctx->available;
    }
    ctx->resolved = true;
    ctx->available = false;

    size_t addr = 0u;
    size_t size = 0u;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0u) {
        return false;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (part == NULL || addr < (size_t)part->address) {
        return false;
    }
    size_t rel = addr - (size_t)part->address;
    if (rel > part->size || size > part->size - rel) {
        return false; /* image_get disagrees with the partition bounds */
    }
    ctx->part = part;
    ctx->image_offset = rel;
    ctx->image_size = size;
    ctx->available = true;
    return true;
}

static size_t honch_esp_coredump_size(void *ctx)
{
    honch_esp_coredump_ctx_t *c = (honch_esp_coredump_ctx_t *)ctx;
    return honch_esp_coredump_resolve(c) ? c->image_size : 0u;
}

static int honch_esp_coredump_read(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    honch_esp_coredump_ctx_t *c = (honch_esp_coredump_ctx_t *)ctx;
    if (!honch_esp_coredump_resolve(c)) {
        return -1;
    }
    /* Overflow-safe bounds check against the snapshot size. */
    if (offset > c->image_size || len > c->image_size - offset) {
        return -1;
    }
    if (esp_partition_read(c->part, c->image_offset + offset, out, len) != ESP_OK) {
        return -1;
    }
    return (int)len;
}

static void honch_esp_coredump_clear(void *ctx)
{
    honch_esp_coredump_ctx_t *c = (honch_esp_coredump_ctx_t *)ctx;
    (void)esp_core_dump_image_erase();
    /* Re-resolve next boot/crash; the image is now gone. */
    c->resolved = false;
    c->available = false;
    c->part = NULL;
    c->image_offset = 0u;
    c->image_size = 0u;
}
#endif
#endif

#if HONCH_ENABLE_LOG_CAPTURE
/* Chained ESP log hook: every error-level (ESP_LOGE) line becomes an automatic
 * $error event, then is always forwarded to the original log handler so normal
 * logging is untouched. The default ESP log line is "E (<ts>) <tag>: <msg>"
 * (optionally wrapped in color escapes); we capture tag + message best-effort. */
static vprintf_like_t s_prev_vprintf = NULL;
/* Whether we actually installed the chained hook this run. Install is gated on
 * the runtime config->enable_error_tracking switch, so we must not restore (and
 * clobber a handler we never replaced) when we skipped it. */
static bool s_log_hook_installed = false;

static int honch_esp_log_vprintf(const char *fmt, va_list args)
{
    /* Per-task guard so our own reporting (and any logging it triggers) cannot
     * recurse back into the hook. */
    static __thread bool in_hook = false;

    if (!in_hook && s_client != NULL && fmt != NULL) {
        va_list copy;
        va_copy(copy, args);
        char line[200];
        int written = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);

        if (written > 0) {
            const char *p = line;
            if (*p == '\033') { /* skip a leading CSI color sequence */
                const char *m = strchr(p, 'm');
                if (m != NULL) {
                    p = m + 1;
                }
            }
            if (*p == 'E') { /* error level only */
                const char *after_ts = strstr(p, ") ");
                if (after_ts != NULL) {
                    const char *tag = after_ts + 2;
                    const char *colon = strchr(tag, ':');
                    if (colon != NULL && colon > tag) {
                        char tag_buf[24];
                        size_t tag_len = (size_t)(colon - tag);
                        if (tag_len >= sizeof(tag_buf)) {
                            tag_len = sizeof(tag_buf) - 1u;
                        }
                        memcpy(tag_buf, tag, tag_len);
                        tag_buf[tag_len] = '\0';

                        const char *msg = colon[1] == ' ' ? colon + 2 : colon + 1;
                        char msg_buf[160];
                        size_t j = 0u;
                        for (const char *q = msg;
                             *q != '\0' && *q != '\033' && *q != '\n' && j + 1u < sizeof(msg_buf);
                             q++) {
                            msg_buf[j++] = *q;
                        }
                        msg_buf[j] = '\0';

                        if (j > 0u) {
                            in_hook = true;
                            (void)honch_core_report_log_error(s_client, tag_buf, msg_buf);
                            in_hook = false;
                        }
                    }
                }
            }
        }
    }

    if (s_prev_vprintf != NULL) {
        return s_prev_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}
#endif

static char s_api_key[HONCH_ESP_API_KEY_MAX_LENGTH + 1u];
static char s_endpoint_url[HONCH_ESP_ENDPOINT_URL_MAX_LENGTH + 1u];
static char s_device_model[HONCH_ESP_DEVICE_MODEL_MAX_LENGTH + 1u];
static char s_firmware_version[HONCH_ESP_FIRMWARE_VERSION_MAX_LENGTH + 1u];
static char s_environment[HONCH_ESP_ENVIRONMENT_MAX_LENGTH + 1u];
static bool (*s_connectivity_callback)(void) = NULL;

static honch_err_t honch_esp_status_to_err(honch_status_t status);

static int honch_esp_connectivity_callback(void *userdata)
{
    (void)userdata;
    return s_connectivity_callback == NULL || s_connectivity_callback() ? 1 : 0;
}

static TickType_t honch_esp_lock_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static bool honch_esp_ensure_mutex(void)
{
    if (s_client_mutex == NULL) {
        portENTER_CRITICAL(&s_client_mutex_init_lock);
        if (s_client_mutex == NULL) {
            s_client_mutex = xSemaphoreCreateMutexStatic(&s_client_mutex_storage);
        }
        portEXIT_CRITICAL(&s_client_mutex_init_lock);
    }
    return s_client_mutex != NULL;
}

static honch_err_t honch_esp_client_lock(void)
{
    if (!honch_esp_ensure_mutex()) {
        return HONCH_ERR_INTERNAL;
    }
    if (xSemaphoreTake(s_client_mutex, honch_esp_lock_ticks(HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return HONCH_ERR_BUSY;
    }
    return HONCH_OK;
}

/*
 * Acquire the client lock for a state-transition finalizer (init_finish,
 * shutdown_finish, shutdown_restore). Unlike honch_esp_client_lock() -- a short
 * cooperative try-lock that fails open so a busy telemetry task never blocks a
 * hot-path API call -- this blocks until the lock is held.
 *
 * These finalizers MUST complete the transition they began: they only flip the
 * ownership flags (no I/O is performed while the lock is held), so the wait is
 * bounded by a few flag writes / a core enter-leave on the other side. Failing
 * open here would strand s_client_initializing or s_client_shutting_down set,
 * after which every later init attempt returns ALREADY_INITIALIZED for the rest
 * of the boot -- the SDK would be bricked.
 */
static void honch_esp_client_lock_blocking(void)
{
    if (!honch_esp_ensure_mutex()) {
        return;
    }
    (void)xSemaphoreTake(s_client_mutex, portMAX_DELAY);
}

static void honch_esp_client_unlock(void)
{
    if (s_client_mutex != NULL) {
        (void)xSemaphoreGive(s_client_mutex);
    }
}

static honch_err_t honch_esp_client_acquire(honch_client_t **out)
{
    if (out == NULL) {
        return HONCH_ERR_INVALID_ARG;
    }
    *out = NULL;

    honch_err_t err = honch_esp_client_lock();
    if (err != HONCH_OK) {
        return err;
    }

    honch_client_t *client = s_client;
    if (client == NULL) {
        honch_esp_client_unlock();
        return HONCH_ERR_NOT_INITIALIZED;
    }

    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_STATUS_OK) {
        honch_esp_client_unlock();
        return honch_esp_status_to_err(status);
    }

    *out = client;
    honch_esp_client_unlock();
    return HONCH_OK;
}

static void honch_esp_client_release(honch_client_t *client)
{
    honch_client_leave(client);
}

static honch_err_t honch_esp_client_detach(honch_client_t **out)
{
    if (out == NULL) {
        return HONCH_ERR_INVALID_ARG;
    }
    *out = NULL;

    honch_err_t err = honch_esp_client_lock();
    if (err != HONCH_OK) {
        return err;
    }

    if (s_client == NULL) {
        honch_esp_client_unlock();
        return HONCH_ERR_NOT_INITIALIZED;
    }

    *out = s_client;
    s_client = NULL;
    s_client_shutting_down = true;
    honch_esp_client_unlock();
    return HONCH_OK;
}

static honch_err_t honch_esp_init_begin(void)
{
    honch_err_t err = honch_esp_client_lock();
    if (err != HONCH_OK) {
        return err;
    }

    if (s_client != NULL || s_client_initializing || s_client_shutting_down) {
        honch_esp_client_unlock();
        return HONCH_ERR_ALREADY_INITIALIZED;
    }

    s_client_initializing = true;
    honch_esp_client_unlock();
    return HONCH_OK;
}

static honch_err_t honch_esp_init_finish(honch_client_t *client)
{
    /* Must complete: blocking lock so s_client_initializing is always cleared. */
    honch_esp_client_lock_blocking();
    s_client = client;
    s_client_initializing = false;
    honch_esp_client_unlock();
    return HONCH_OK;
}

static void honch_esp_shutdown_finish(void)
{
    /* Must complete: blocking lock so s_client_shutting_down is always cleared. */
    honch_esp_client_lock_blocking();
    s_client_shutting_down = false;
    honch_esp_client_unlock();
}

static void honch_esp_shutdown_restore(honch_client_t *client)
{
    /* Must complete: blocking lock so a failed shutdown fully rolls back. */
    honch_esp_client_lock_blocking();
    s_client = client;
    s_client_shutting_down = false;
    honch_esp_client_unlock();
}

bool honch_esp_is_initialized(void)
{
    bool initialized = false;
    if (honch_esp_client_lock() != HONCH_OK) {
        return false;
    }
    initialized = s_client != NULL;
    honch_esp_client_unlock();
    return initialized;
}

static honch_err_t honch_esp_status_to_err(honch_status_t status)
{
    switch (status) {
        case HONCH_STATUS_OK:
            return HONCH_OK;
        case HONCH_STATUS_ERROR_INVALID_ARGUMENT:
            return HONCH_ERR_INVALID_ARG;
        case HONCH_STATUS_ERROR_OUT_OF_MEMORY:
            return HONCH_ERR_NO_MEM;
        case HONCH_STATUS_ERROR_IO:
            return HONCH_ERR_IO;
        case HONCH_STATUS_ERROR_TRANSPORT:
        case HONCH_STATUS_ERROR_RATE_LIMITED:
        case HONCH_STATUS_ERROR_SERVER:
        case HONCH_STATUS_ERROR_REJECTED:
            return HONCH_ERR_TRANSPORT;
        case HONCH_STATUS_ERROR_NOT_INITIALIZED:
            return HONCH_ERR_NOT_INITIALIZED;
        case HONCH_STATUS_ERROR_ALREADY_INITIALIZED:
            return HONCH_ERR_ALREADY_INITIALIZED;
        case HONCH_STATUS_ERROR_QUEUE_FULL:
            return HONCH_ERR_QUEUE_FULL;
        case HONCH_STATUS_ERROR_TIMEOUT:
            return HONCH_ERR_TIMEOUT;
        case HONCH_STATUS_ERROR_BUSY:
            return HONCH_ERR_BUSY;
        case HONCH_STATUS_ERROR_NOT_SUPPORTED:
            return HONCH_ERR_NOT_SUPPORTED;
        case HONCH_STATUS_ERROR_OFFLINE:
            return HONCH_ERR_OFFLINE;
        case HONCH_STATUS_ERROR_INTERNAL:
        default:
            return HONCH_ERR_INTERNAL;
    }
}

static void honch_esp_clear_config_state(void)
{
    s_api_key[0] = '\0';
    s_endpoint_url[0] = '\0';
    s_device_model[0] = '\0';
    s_firmware_version[0] = '\0';
    s_environment[0] = '\0';
    s_connectivity_callback = NULL;
}

static honch_err_t honch_esp_copy_static_config_string(char *dest, size_t dest_size, const char *value)
{
    if (dest == NULL || dest_size == 0u || value == NULL) {
        return HONCH_ERR_INVALID_ARG;
    }

    int written = snprintf(dest, dest_size, "%s", value);
    if (written < 0 || written >= (int)dest_size) {
        return HONCH_ERR_INVALID_ARG;
    }
    return HONCH_OK;
}

static honch_err_t honch_esp_copy_config(const honch_config_t *config)
{
    const char *environment = config->environment != NULL && config->environment[0] != '\0'
        ? config->environment
        : "production";
    const char *host = config->host != NULL && config->host[0] != '\0'
        ? config->host
        : "https://i.honch.io";
    honch_err_t err = honch_esp_copy_static_config_string(s_api_key, sizeof(s_api_key), config->api_key);
    if (err == HONCH_OK) {
        err = honch_esp_copy_static_config_string(s_endpoint_url, sizeof(s_endpoint_url), host);
    }
    if (err == HONCH_OK) {
        err = honch_esp_copy_static_config_string(s_device_model, sizeof(s_device_model), config->device_model);
    }
    if (err == HONCH_OK) {
        err = honch_esp_copy_static_config_string(
            s_firmware_version,
            sizeof(s_firmware_version),
            config->firmware_version);
    }
    if (err == HONCH_OK) {
        err = honch_esp_copy_static_config_string(s_environment, sizeof(s_environment), environment);
    }
    if (err != HONCH_OK) {
        return err;
    }

    s_connectivity_callback = config->connectivity_callback;
    return HONCH_OK;
}

honch_err_t honch_init(const honch_config_t *config)
{
    honch_err_t err = honch_esp_init_begin();
    if (err != HONCH_OK) {
        return err;
    }

    if (config == NULL || config->api_key == NULL ||
        config->device_model == NULL || config->firmware_version == NULL ||
        (config->event_queue_ops == NULL && (config->event_buffer == NULL || config->event_buffer_size == 0u))) {
        honch_esp_init_finish(NULL);
        return HONCH_ERR_INVALID_ARG;
    }

    err = honch_esp_copy_config(config);
    if (err != HONCH_OK) {
        honch_esp_init_finish(NULL);
        return err;
    }

    honch_platform_ops_t platform_ops;
    honch_event_queue_ops_t event_queue_ops;
    honch_transport_ops_t transport_ops;

    honch_status_t status = honch_esp_platform_ops_init(&platform_ops, &s_platform_ctx);
    if (status != HONCH_STATUS_OK) {
        honch_esp_clear_config_state();
        honch_esp_init_finish(NULL);
        return honch_esp_status_to_err(status);
    }
    if (config->event_queue_ops != NULL) {
        event_queue_ops = *config->event_queue_ops;
    } else {
        /* The ESP public config does not expose the queue depth cap, so the core
         * uses HONCH_DEFAULT_MAX_QUEUED_EVENTS. Size the entry metadata to that
         * same cap rather than buffer_size/16, which can be many times larger. */
        status = honch_esp_event_queue_ops_init(
            &event_queue_ops,
            &s_storage_ctx,
            config->event_buffer,
            config->event_buffer_size,
            HONCH_DEFAULT_MAX_QUEUED_EVENTS);
        if (status != HONCH_STATUS_OK) {
            honch_esp_platform_ops_deinit(&s_platform_ctx);
            honch_esp_clear_config_state();
            honch_esp_init_finish(NULL);
            return honch_esp_status_to_err(status);
        }
    }
    status = honch_esp_transport_ops_init(&transport_ops, &s_transport_ctx, config->transport_timeout_ms);
    if (status != HONCH_STATUS_OK) {
        honch_esp_event_queue_ops_deinit(&s_storage_ctx);
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_config_state();
        honch_esp_init_finish(NULL);
        return honch_esp_status_to_err(status);
    }

    honch_core_config_t core_config = {0};
    core_config.api_key = s_api_key;
    core_config.endpoint_url = s_endpoint_url;
    core_config.device_model = s_device_model;
    core_config.firmware_version = s_firmware_version;
    core_config.environment = s_environment;
    core_config.sdk_platform = "esp-idf";
    core_config.queue_directory = "";
    if (config->flush_event_threshold > 0u) {
        core_config.batch_size = config->flush_event_threshold > HONCH_MAX_BATCH_SIZE ?
            HONCH_MAX_BATCH_SIZE :
            config->flush_event_threshold;
    }
    core_config.flush_interval_seconds = config->flush_interval_seconds;
    core_config.flush_min_interval_ms = config->flush_min_interval_ms;
    core_config.flush_event_threshold = config->flush_event_threshold;
    core_config.flush_max_batches = config->flush_max_batches;
    core_config.shutdown_flush_max_batches = config->shutdown_flush_max_batches;
    core_config.transport_timeout_ms = config->transport_timeout_ms;
    core_config.battery_callback = config->battery_callback;
    core_config.battery_low_threshold =
        config->battery_low_threshold > 0 ? config->battery_low_threshold : 15;
    core_config.connectivity_callback = honch_esp_connectivity_callback;
    core_config.connectivity_userdata = NULL;
    core_config.platform = &platform_ops;
    core_config.state_storage = config->state_storage_ops;
    core_config.event_queue = &event_queue_ops;
    core_config.transport = &transport_ops;
#if HONCH_ENABLE_ERROR_TRACKING
    bool should_emit_fault_snapshot =
        config->enable_error_tracking && !s_fault_snapshot_consumed;
    honch_crash_report_t crash_report = honch_esp_crash_report(
        should_emit_fault_snapshot && config->enable_crash_symbolication);
    core_config.crash_report = should_emit_fault_snapshot ? &crash_report : NULL;
#if HONCH_ESP_COMPAT_HAS_COREDUMP
    core_config.crash_uploaded_callback = honch_esp_crash_uploaded;
    /* Stream the raw ELF coredump from flash to Capture after the $crash. The
     * core reads this source's size() once, streams it in bounded chunks, and
     * calls clear() (the single erase) only after the blob's final ack. Only
     * armed when a crash is actually being reported this boot. */
    if (core_config.crash_report != NULL) {
        s_coredump_ctx = (honch_esp_coredump_ctx_t){0};
        s_coredump_source.size = honch_esp_coredump_size;
        s_coredump_source.read = honch_esp_coredump_read;
        s_coredump_source.clear = honch_esp_coredump_clear;
        s_coredump_source.ctx = &s_coredump_ctx;
        core_config.coredump_source = &s_coredump_source;
    }
#endif
#else
    core_config.crash_report = NULL;
#endif

    honch_client_t *next = NULL;
    status = honch_core_init(&next, &core_config);
    if (status != HONCH_STATUS_OK) {
        honch_esp_transport_ops_deinit(&s_transport_ctx);
        honch_esp_event_queue_ops_deinit(&s_storage_ctx);
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_config_state();
        honch_esp_init_finish(NULL);
        ESP_LOGE(TAG, "Core init failed: %s", honch_status_string(status));
        return honch_esp_status_to_err(status);
    }

    honch_esp_init_finish(next);
#if HONCH_ENABLE_ERROR_TRACKING
    if (should_emit_fault_snapshot) {
        s_fault_snapshot_consumed = true;
    }
#endif
#if HONCH_ENABLE_LOG_CAPTURE
    /* Install the chained log hook so ESP_LOGE lines become automatic $error
     * events — but only when error tracking is enabled at runtime, so the
     * config->enable_error_tracking switch governs log capture and the crash
     * snapshot consistently (a caller that disables error tracking gets neither).
     * esp_log_set_vprintf returns the previous handler to forward to. */
    if (config->enable_error_tracking) {
        s_prev_vprintf = esp_log_set_vprintf(honch_esp_log_vprintf);
        s_log_hook_installed = true;
    }
#endif
    return HONCH_OK;
}

honch_err_t honch_shutdown(void)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_detach(&client);
    if (err != HONCH_OK) {
        return err;
    }

    honch_status_t status = honch_core_shutdown(client);
    if (status == HONCH_STATUS_ERROR_BUSY) {
        honch_esp_shutdown_restore(client);
        return HONCH_ERR_BUSY;
    }
#if HONCH_ENABLE_LOG_CAPTURE
    /* Restore the previous log handler before the client is gone — only if we
     * actually installed our hook (when error tracking is off at runtime we
     * never replaced the handler, so we must not clobber it here). */
    if (s_log_hook_installed) {
        (void)esp_log_set_vprintf(s_prev_vprintf != NULL ? s_prev_vprintf : &vprintf);
        s_prev_vprintf = NULL;
        s_log_hook_installed = false;
    }
#endif
    honch_esp_transport_ops_deinit(&s_transport_ctx);
    honch_esp_event_queue_ops_deinit(&s_storage_ctx);
    honch_esp_platform_ops_deinit(&s_platform_ctx);
    honch_esp_clear_config_state();
    honch_esp_shutdown_finish();
    return honch_esp_status_to_err(status);
}

honch_err_t honch_track(const char *event, const honch_property_t *properties, size_t property_count)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_track(client, event, properties, property_count);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_identify(const char *distinct_id, const honch_property_t *properties, size_t property_count)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_identify(client, distinct_id, properties, property_count);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_set_property(const char *key, honch_value_t value)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_set_property(client, key, value);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_session_start(const char *session_name)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_session_start(client, session_name);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_session_end(void)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_session_end(client);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_flush(void)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_flush(client);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_tick(void)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_tick(client);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_reset(void)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_reset(client);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

const char *honch_get_device_id(void)
{
    honch_client_t *client = NULL;
    if (honch_esp_client_acquire(&client) != HONCH_OK) {
        return NULL;
    }
    const char *device_id = honch_core_get_device_id(client);
    honch_esp_client_release(client);
    return device_id;
}

honch_err_t honch_get_queue_stats(honch_queue_stats_t *stats)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_get_queue_stats(client, stats);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_get_last_error(honch_error_detail_t *detail)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_get_last_error(client, detail);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}
