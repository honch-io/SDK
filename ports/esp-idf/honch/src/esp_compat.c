// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#define HONCH_CORE_NO_SHORT_STATUS_NAMES
#include "honch/core/honch.h"

#include "esp_core_adapter.h"
#include "honch_internal.h"

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

static honch_err_t honch_esp_client_lock(void)
{
    if (s_client_mutex == NULL) {
        portENTER_CRITICAL(&s_client_mutex_init_lock);
        if (s_client_mutex == NULL) {
            s_client_mutex = xSemaphoreCreateMutexStatic(&s_client_mutex_storage);
        }
        portEXIT_CRITICAL(&s_client_mutex_init_lock);
    }
    if (s_client_mutex == NULL) {
        return HONCH_ERR_INTERNAL;
    }
    if (xSemaphoreTake(s_client_mutex, honch_esp_lock_ticks(HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return HONCH_ERR_BUSY;
    }
    return HONCH_OK;
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
    honch_err_t err = honch_esp_client_lock();
    if (err != HONCH_OK) {
        return err;
    }
    s_client = client;
    s_client_initializing = false;
    honch_esp_client_unlock();
    return HONCH_OK;
}

static void honch_esp_shutdown_finish(void)
{
    if (honch_esp_client_lock() != HONCH_OK) {
        return;
    }
    s_client_shutting_down = false;
    honch_esp_client_unlock();
}

static void honch_esp_shutdown_restore(honch_client_t *client)
{
    if (honch_esp_client_lock() != HONCH_OK) {
        return;
    }
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
    honch_err_t err = honch_esp_copy_static_config_string(s_api_key, sizeof(s_api_key), config->api_key);
    if (err == HONCH_OK) {
        err = honch_esp_copy_static_config_string(s_endpoint_url, sizeof(s_endpoint_url), config->host);
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

    if (config == NULL || config->api_key == NULL || config->host == NULL ||
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
        status = honch_esp_event_queue_ops_init(
            &event_queue_ops,
            &s_storage_ctx,
            config->event_buffer,
            config->event_buffer_size);
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
    core_config.enable_error_tracking = config->enable_error_tracking;
    honch_fault_snapshot_t fault_snapshot = honch_esp_fault_snapshot(
        config->enable_error_tracking && config->enable_crash_symbolication);
    core_config.fault_snapshot = &fault_snapshot;

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
