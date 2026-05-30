// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#define HONCH_CORE_NO_SHORT_STATUS_NAMES
#include "honch/core/honch.h"

#include "esp_core_adapter.h"
#include "honch_internal.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "honch";

static honch_client_t *s_client = NULL;
static bool s_client_initializing = false;
static bool s_client_shutting_down = false;
static pthread_mutex_t s_client_mutex = PTHREAD_MUTEX_INITIALIZER;
static honch_esp_platform_t s_platform_ctx;
static honch_esp_storage_t s_storage_ctx;
static honch_esp_transport_t s_transport_ctx;

static char s_api_key[128];
static char s_endpoint_url[280];
static char s_device_model[64];
static char s_firmware_version[32];
static char s_environment[32];

const char *g_honch_api_key = NULL;
const char *g_honch_device_model = NULL;
const char *g_honch_firmware_version = NULL;
const char *g_honch_environment = "production";
int (*g_honch_battery_callback)(void) = NULL;
int g_honch_battery_low_threshold = 15;
volatile bool g_honch_connected = false;

extern void honch_esp_gpio_shutdown_hook(void) __attribute__((weak));

static honch_err_t honch_esp_status_to_err(honch_status_t status);

static honch_err_t honch_esp_client_acquire(honch_client_t **out)
{
    if (out == NULL) {
        return HONCH_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (pthread_mutex_lock(&s_client_mutex) != 0) {
        return HONCH_ERR_INTERNAL;
    }

    honch_client_t *client = s_client;
    if (client == NULL) {
        (void)pthread_mutex_unlock(&s_client_mutex);
        return HONCH_ERR_NOT_INITIALIZED;
    }

    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_STATUS_OK) {
        (void)pthread_mutex_unlock(&s_client_mutex);
        return honch_esp_status_to_err(status);
    }

    *out = client;
    (void)pthread_mutex_unlock(&s_client_mutex);
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

    if (pthread_mutex_lock(&s_client_mutex) != 0) {
        return HONCH_ERR_INTERNAL;
    }

    if (s_client == NULL) {
        (void)pthread_mutex_unlock(&s_client_mutex);
        return HONCH_ERR_NOT_INITIALIZED;
    }

    *out = s_client;
    s_client = NULL;
    s_client_shutting_down = true;
    (void)pthread_mutex_unlock(&s_client_mutex);
    return HONCH_OK;
}

static honch_err_t honch_esp_init_begin(void)
{
    if (pthread_mutex_lock(&s_client_mutex) != 0) {
        return HONCH_ERR_INTERNAL;
    }

    if (s_client != NULL || s_client_initializing || s_client_shutting_down) {
        (void)pthread_mutex_unlock(&s_client_mutex);
        return HONCH_ERR_ALREADY_INITIALIZED;
    }

    s_client_initializing = true;
    (void)pthread_mutex_unlock(&s_client_mutex);
    return HONCH_OK;
}

static void honch_esp_init_finish(honch_client_t *client)
{
    if (pthread_mutex_lock(&s_client_mutex) != 0) {
        return;
    }

    s_client = client;
    s_client_initializing = false;
    (void)pthread_mutex_unlock(&s_client_mutex);
}

static void honch_esp_shutdown_finish(void)
{
    if (pthread_mutex_lock(&s_client_mutex) != 0) {
        return;
    }

    s_client_shutting_down = false;
    (void)pthread_mutex_unlock(&s_client_mutex);
}

bool honch_esp_is_initialized(void)
{
    bool initialized = false;
    if (pthread_mutex_lock(&s_client_mutex) == 0) {
        initialized = s_client != NULL;
        (void)pthread_mutex_unlock(&s_client_mutex);
    }
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
            return HONCH_ERR_NVS;
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
        case HONCH_STATUS_ERROR_INTERNAL:
        default:
            return HONCH_ERR_INTERNAL;
    }
}

static honch_durability_mode_t honch_esp_resolve_durability_mode(honch_durability_mode_t durability_mode)
{
    switch (durability_mode) {
        case HONCH_DURABILITY_SYNC_ALWAYS:
        case HONCH_DURABILITY_OS_BUFFERED:
            return durability_mode;
        default:
            return HONCH_DURABILITY_OS_BUFFERED;
    }
}

static void honch_esp_clear_legacy_globals(void)
{
    g_honch_api_key = NULL;
    g_honch_device_model = NULL;
    g_honch_firmware_version = NULL;
    g_honch_environment = "production";
    g_honch_battery_callback = NULL;
    g_honch_battery_low_threshold = 15;
    g_honch_connected = false;
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

    g_honch_api_key = s_api_key;
    g_honch_device_model = s_device_model;
    g_honch_firmware_version = s_firmware_version;
    g_honch_environment = s_environment;
    g_honch_battery_callback = config->battery_callback;
    g_honch_battery_low_threshold =
        config->battery_low_threshold > 0 ? config->battery_low_threshold : 15;
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
        config->event_buffer == NULL || config->event_buffer_size == 0u) {
        honch_esp_init_finish(NULL);
        return HONCH_ERR_INVALID_ARG;
    }

    err = honch_esp_copy_config(config);
    if (err != HONCH_OK) {
        honch_esp_init_finish(NULL);
        return err;
    }

    honch_platform_ops_t platform_ops;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport_ops;

    honch_status_t status = honch_esp_platform_ops_init(&platform_ops, &s_platform_ctx);
    if (status != HONCH_STATUS_OK) {
        honch_esp_clear_legacy_globals();
        honch_esp_init_finish(NULL);
        return honch_esp_status_to_err(status);
    }
    status = honch_esp_storage_ops_init(&storage_ops, &s_storage_ctx);
    if (status != HONCH_STATUS_OK) {
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_legacy_globals();
        honch_esp_init_finish(NULL);
        return honch_esp_status_to_err(status);
    }
    status = honch_esp_storage_use_ram_queue(
        &s_storage_ctx,
        config->event_buffer,
        config->event_buffer_size);
    if (status != HONCH_STATUS_OK) {
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_legacy_globals();
        honch_esp_init_finish(NULL);
        return honch_esp_status_to_err(status);
    }
    status = honch_esp_transport_ops_init(&transport_ops, &s_transport_ctx);
    if (status != HONCH_STATUS_OK) {
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_legacy_globals();
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
    core_config.queue_directory = "nvs";
    if (config->flush_event_threshold > 0u) {
        core_config.batch_size = config->flush_event_threshold > HONCH_MAX_BATCH_SIZE ?
            HONCH_MAX_BATCH_SIZE :
            config->flush_event_threshold;
    }
    core_config.flush_interval_seconds = config->flush_interval_seconds;
    core_config.flush_event_threshold = config->flush_event_threshold;
    core_config.battery_callback = config->battery_callback;
    core_config.battery_low_threshold = g_honch_battery_low_threshold;
    core_config.durability_mode = honch_esp_resolve_durability_mode(config->durability_mode);
    core_config.disable_background_flush = 0;
    core_config.platform = &platform_ops;
    core_config.storage = &storage_ops;
    core_config.transport = &transport_ops;

    honch_client_t *next = NULL;
    status = honch_core_init(&next, &core_config);
    if (status != HONCH_STATUS_OK) {
        honch_esp_platform_ops_deinit(&s_platform_ctx);
        honch_esp_clear_legacy_globals();
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

    if (honch_esp_gpio_shutdown_hook != NULL) {
        honch_esp_gpio_shutdown_hook();
    }
    honch_status_t status = honch_core_shutdown(client);
    honch_esp_platform_ops_deinit(&s_platform_ctx);
    honch_esp_clear_legacy_globals();
    honch_esp_shutdown_finish();
    return honch_esp_status_to_err(status);
}

honch_err_t honch_track(const char *event, const char *properties_json)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_track(client, event, properties_json);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_identify(const char *distinct_id, const char *properties_json)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_identify(client, distinct_id, properties_json);
    honch_esp_client_release(client);
    return honch_esp_status_to_err(status);
}

honch_err_t honch_set_property(const char *key, const char *value_json)
{
    honch_client_t *client = NULL;
    honch_err_t err = honch_esp_client_acquire(&client);
    if (err != HONCH_OK) {
        return err;
    }
    honch_status_t status = honch_core_set_property(client, key, value_json);
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
