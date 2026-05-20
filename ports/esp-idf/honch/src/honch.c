// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"
#include "identity.h"
#include "queue.h"
#include "encoder.h"
#include "transport.h"
#include "scheduler.h"
#include "esp_gpio_adapter.h"
#include "lifecycle.h"
#include "perf.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "honch";

// Global config state shared with encoder and scheduler
const char *g_honch_api_key = NULL;
const char *g_honch_device_model = NULL;
const char *g_honch_firmware_version = NULL;
const char *g_honch_environment = "production";
int (*g_honch_battery_callback)(void) = NULL;
int g_honch_battery_low_threshold = 15;
volatile bool g_honch_connected = false;

static bool s_initialized = false;
static SemaphoreHandle_t s_api_mutex = NULL;
static uint32_t s_flush_threshold = 30;

// Stored copies of config strings (caller-owned originals may go out of scope)
static char s_api_key[128];
static char s_host[256];
static char s_device_model[64];
static char s_firmware_version[32];
static char s_environment[32];

honch_err_t honch_init(const honch_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return HONCH_ERR_ALREADY_INITIALIZED;
    }

    if (!config || !config->api_key || !config->host ||
        !config->device_model || !config->firmware_version ||
        !config->event_buffer || config->event_buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid configuration");
        return HONCH_ERR_INVALID_ARG;
    }

    // Copy config strings
    strncpy(s_api_key, config->api_key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    g_honch_api_key = s_api_key;

    strncpy(s_host, config->host, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';

    strncpy(s_device_model, config->device_model, sizeof(s_device_model) - 1);
    s_device_model[sizeof(s_device_model) - 1] = '\0';
    g_honch_device_model = s_device_model;

    strncpy(s_firmware_version, config->firmware_version, sizeof(s_firmware_version) - 1);
    s_firmware_version[sizeof(s_firmware_version) - 1] = '\0';
    g_honch_firmware_version = s_firmware_version;

    if (config->environment && strlen(config->environment) > 0) {
        strncpy(s_environment, config->environment, sizeof(s_environment) - 1);
        s_environment[sizeof(s_environment) - 1] = '\0';
    } else {
        strcpy(s_environment, "production");
    }
    g_honch_environment = s_environment;

    g_honch_battery_callback = config->battery_callback;
    g_honch_battery_low_threshold = config->battery_low_threshold > 0
        ? config->battery_low_threshold : 15;

    uint32_t flush_interval = config->flush_interval_seconds > 0
        ? config->flush_interval_seconds : 60;
    s_flush_threshold = config->flush_event_threshold > 0
        ? config->flush_event_threshold : 30;

    // Create API mutex
    s_api_mutex = xSemaphoreCreateMutex();
    if (!s_api_mutex) {
        return HONCH_ERR_NO_MEM;
    }

    // Initialize subsystems
    honch_err_t err;

    err = honch_identity_init();
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Identity init failed");
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
        return err;
    }

    err = honch_queue_init();
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Queue init failed");
        honch_identity_deinit();
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
        return err;
    }

    err = honch_transport_init(s_host);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Transport init failed");
        honch_queue_deinit();
        honch_identity_deinit();
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
        return err;
    }

    err = honch_gpio_init();
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "GPIO init failed");
        honch_transport_deinit();
        honch_queue_deinit();
        honch_identity_deinit();
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
        return err;
    }

    err = honch_lifecycle_init();
    if (err != HONCH_OK) {
        ESP_LOGW(TAG, "Lifecycle init failed (non-fatal)");
    }

    err = honch_scheduler_init(flush_interval, s_flush_threshold);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Scheduler init failed");
        honch_lifecycle_deinit();
        honch_gpio_deinit();
        honch_transport_deinit();
        honch_queue_deinit();
        honch_identity_deinit();
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
        return err;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (device_id: %s)", honch_identity_get_device_id());

    // Check for firmware update
    honch_lifecycle_check_firmware(s_firmware_version);

    // Emit boot event
    honch_lifecycle_emit_boot();

    return HONCH_OK;
}

honch_err_t honch_shutdown(void)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    // Emit shutdown event
    honch_lifecycle_emit_shutdown();

    // Synchronous flush with 5-second timeout
    ESP_LOGI(TAG, "Flushing remaining events before shutdown...");
    honch_scheduler_flush_sync(5000);

    // Tear down in reverse order
    honch_scheduler_deinit();
    honch_lifecycle_deinit();
    honch_gpio_deinit();
    honch_transport_deinit();
    honch_queue_deinit();
    honch_identity_deinit();

    if (s_api_mutex) {
        vSemaphoreDelete(s_api_mutex);
        s_api_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Shutdown complete");
    return HONCH_OK;
}

honch_err_t honch_track(const char *event, const char *properties_json)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }
    if (!event) {
        return HONCH_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    const char *distinct_id = honch_identity_get_distinct_id();
    honch_event_runtime_t runtime = {0};
    honch_event_runtime_collect(&runtime);
    honch_payload_t encoded = {0};
    int64_t encode_start_us = HONCH_PERF_NOW_US();
    honch_err_t encode_err = honch_encode_event(event, distinct_id, properties_json, &runtime, &encoded);
    int64_t encode_us = HONCH_PERF_ELAPSED_US(encode_start_us);
    size_t encoded_len = encoded.len;

    xSemaphoreGive(s_api_mutex);

    if (encode_err != HONCH_OK) {
        HONCH_PERF_LOG("HONCH_PERF_TRACK",
                       "event=%s result=encode_error code=%d total_us=%" PRId64
                       " encode_us=%" PRId64 " heap_before=%" PRIu32 " heap_after=%" PRIu32,
                       event,
                       encode_err,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       encode_us,
                       heap_before,
                       HONCH_PERF_HEAP_FREE());
        return encode_err;
    }

    int64_t queue_start_us = HONCH_PERF_NOW_US();
    honch_err_t err = honch_queue_push(&encoded);
    int64_t queue_us = HONCH_PERF_ELAPSED_US(queue_start_us);
    // queue_push takes ownership and frees encoded.

    if (err == HONCH_OK) {
        // Check battery while we're at it
        if (runtime.battery_known) {
            honch_lifecycle_check_battery(runtime.battery_level);
        }

        // Notify worker if threshold crossed
        if (honch_queue_depth() >= s_flush_threshold) {
            honch_scheduler_notify();
        }
    }

    HONCH_PERF_LOG("HONCH_PERF_TRACK",
                   "event=%s result=%d total_us=%" PRId64 " encode_us=%" PRId64
                   " queue_us=%" PRId64 " encoded_bytes=%u heap_before=%" PRIu32
                   " heap_after=%" PRIu32 " heap_min=%" PRIu32,
                   event,
                   err,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   encode_us,
                   queue_us,
                   (unsigned)encoded_len,
                   heap_before,
                   HONCH_PERF_HEAP_FREE(),
                   HONCH_PERF_HEAP_MIN());

    return err;
}

honch_err_t honch_identify(const char *distinct_id, const char *properties_json)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }
    if (!distinct_id) {
        return HONCH_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    honch_err_t err = honch_identity_set_distinct_id(distinct_id);
    if (err != HONCH_OK) {
        xSemaphoreGive(s_api_mutex);
        return err;
    }

    xSemaphoreGive(s_api_mutex);

    // Track an $identify event
    return honch_track("$identify", properties_json);
}

honch_err_t honch_set_property(const char *key, const char *value_json)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }
    if (!key) {
        return HONCH_ERR_INVALID_ARG;
    }

    // Build a JSON object with the key-value pair
    // value_json can be a raw JSON value like "\"hello\"" or "42" or "{...}"
    size_t buf_size = strlen(key) + (value_json ? strlen(value_json) : 4) + 16;
    char *props = malloc(buf_size);
    if (!props) {
        return HONCH_ERR_NO_MEM;
    }

    snprintf(props, buf_size, "{\"%s\":%s}", key, value_json ? value_json : "null");

    honch_err_t err = honch_track("$set_property", props);
    free(props);
    return err;
}

honch_err_t honch_session_start(const char *session_name)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    // If a session is already active, end it first
    if (honch_identity_get_session_id() != NULL) {
        ESP_LOGW(TAG, "Session already active, ending it first");
        xSemaphoreGive(s_api_mutex);
        honch_session_end();
        xSemaphoreTake(s_api_mutex, portMAX_DELAY);
    }

    honch_err_t err = honch_identity_start_session();
    xSemaphoreGive(s_api_mutex);

    if (err != HONCH_OK) {
        return err;
    }

    // Emit $session_start event
    if (session_name) {
        size_t buf_size = strlen(session_name) + 32;
        char *props = malloc(buf_size);
        if (props) {
            snprintf(props, buf_size, "{\"session_name\":\"%s\"}", session_name);
            err = honch_track("$session_start", props);
            free(props);
        } else {
            err = honch_track("$session_start", NULL);
        }
    } else {
        err = honch_track("$session_start", NULL);
    }

    return err;
}

honch_err_t honch_session_end(void)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    if (honch_identity_get_session_id() == NULL) {
        xSemaphoreGive(s_api_mutex);
        ESP_LOGW(TAG, "No active session to end");
        return HONCH_OK;
    }

    xSemaphoreGive(s_api_mutex);

    // Emit $session_end while session is still active (so it gets the session_id)
    honch_err_t err = honch_track("$session_end", NULL);

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);
    honch_identity_end_session();
    xSemaphoreGive(s_api_mutex);

    return err;
}

honch_err_t honch_flush(void)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    honch_scheduler_notify();
    return HONCH_OK;
}

honch_err_t honch_reset(void)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    // Emit $device_reset before clearing state
    honch_track("$device_reset", NULL);

    xSemaphoreTake(s_api_mutex, portMAX_DELAY);
    honch_err_t err = honch_identity_reset();
    xSemaphoreGive(s_api_mutex);

    if (err != HONCH_OK) {
        return err;
    }

    // Clear the event queue
    return honch_queue_clear();
}

const char *honch_get_device_id(void)
{
    if (!s_initialized) {
        return NULL;
    }
    return honch_identity_get_device_id();
}

honch_err_t honch_track_gpio(gpio_num_t pin, const char *event_name, honch_gpio_mode_t mode)
{
    if (!s_initialized) {
        return HONCH_ERR_NOT_INITIALIZED;
    }
    if (!event_name) {
        return HONCH_ERR_INVALID_ARG;
    }

    return honch_gpio_register(pin, event_name, mode);
}
