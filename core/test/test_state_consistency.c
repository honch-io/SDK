#include "honch_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_state_storage {
    char device_id[64];
    char distinct_id[64];
    char firmware_version[64];
    size_t queue_depth;
    honch_status_t queue_push_status;
    const char *state_size_fault_key;
    const char *state_read_overreport_key;
    int fail_distinct_id_set;
} fake_state_storage_t;

static uint64_t fake_now_ms(void *ctx)
{
    (void)ctx;
    return 1000u;
}

static honch_status_t fake_random_bytes(void *ctx, uint8_t *buffer, size_t buffer_size)
{
    (void)ctx;
    for (size_t i = 0u; i < buffer_size; i++) {
        buffer[i] = (uint8_t)(0xa0u + i);
    }
    return HONCH_OK;
}

static char *fake_state_value(fake_state_storage_t *storage, const char *key)
{
    if (strcmp(key, "device_id") == 0) {
        return storage->device_id;
    }
    if (strcmp(key, "distinct_id") == 0) {
        return storage->distinct_id;
    }
    if (strcmp(key, "firmware_version") == 0) {
        return storage->firmware_version;
    }
    return NULL;
}

static honch_status_t fake_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    char *value = fake_state_value(storage, key);
    if (value == NULL || buffer_size == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (storage->state_size_fault_key != NULL && strcmp(key, storage->state_size_fault_key) == 0) {
        *buffer_size = SIZE_MAX;
        return HONCH_OK;
    }

    if (storage->state_read_overreport_key != NULL && strcmp(key, storage->state_read_overreport_key) == 0) {
        if (buffer == NULL) {
            *buffer_size = 3u;
            return HONCH_OK;
        }
        if (*buffer_size < 3u) {
            *buffer_size = 3u;
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        memcpy(buffer, "abc", 3u);
        *buffer_size = 4u;
        return HONCH_OK;
    }

    size_t length = strlen(value);
    if (buffer == NULL || *buffer_size < length) {
        *buffer_size = length;
        return buffer == NULL ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
    }

    memcpy(buffer, value, length);
    *buffer_size = length;
    return HONCH_OK;
}

static honch_status_t fake_state_set(void *ctx, const char *key, const uint8_t *data, size_t data_size)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    char *value = fake_state_value(storage, key);
    if (value == NULL || data_size >= 64u || (data == NULL && data_size > 0u)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "distinct_id") == 0 && storage->fail_distinct_id_set) {
        return HONCH_ERROR_IO;
    }

    memcpy(value, data, data_size);
    value[data_size] = '\0';
    return HONCH_OK;
}

static honch_status_t fake_state_delete(void *ctx, const char *key)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    char *value = fake_state_value(storage, key);
    if (value == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    value[0] = '\0';
    return HONCH_OK;
}

static honch_status_t fake_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    (void)event;
    (void)event_size;
    (void)sequence;
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage->queue_push_status != HONCH_OK) {
        return storage->queue_push_status;
    }
    return HONCH_OK;
}

static honch_status_t fake_queue_clear(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_depth = 0u;
    return HONCH_OK;
}

static honch_status_t fake_queue_depth(void *ctx, size_t *depth)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    *depth = storage->queue_depth;
    return HONCH_OK;
}

static honch_status_t fake_post_chunk(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    (void)ctx;
    (void)endpoint_url;
    (void)api_key;
    (void)stream_id;
    (void)body;
    (void)body_size;
    *result = HONCH_TRANSPORT_ACCEPTED;
    return HONCH_OK;
}

static honch_client_t *g_callback_lock_client = NULL;
static int g_battery_callback_saw_unlocked_mutex = 0;
static int g_auto_properties_callback_saw_unlocked_mutex = 0;

static int lock_observing_battery_callback(void)
{
    if (g_callback_lock_client == NULL) {
        return 50;
    }

    int lock_status = pthread_mutex_trylock(&g_callback_lock_client->mutex);
    if (lock_status == 0) {
        g_battery_callback_saw_unlocked_mutex = 1;
        pthread_mutex_unlock(&g_callback_lock_client->mutex);
    } else {
        assert(lock_status == EBUSY);
    }
    return 50;
}

static honch_status_t lock_observing_auto_properties_callback(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    (void)userdata;
    if (g_callback_lock_client != NULL) {
        int lock_status = pthread_mutex_trylock(&g_callback_lock_client->mutex);
        if (lock_status == 0) {
            g_auto_properties_callback_saw_unlocked_mutex = 1;
            pthread_mutex_unlock(&g_callback_lock_client->mutex);
        } else {
            assert(lock_status == EBUSY);
        }
    }

    return sink(sink_ctx, "$wifi_rssi", "-42");
}

static honch_core_config_t fake_config(
    fake_state_storage_t *storage,
    honch_platform_ops_t *platform,
    honch_storage_ops_t *storage_ops,
    honch_transport_ops_t *transport)
{
    *platform = (honch_platform_ops_t) {
        .now_ms = fake_now_ms,
        .uptime_ms = fake_now_ms,
        .random_bytes = fake_random_bytes,
        .ctx = storage
    };
    *storage_ops = (honch_storage_ops_t) {
        .state_get = fake_state_get,
        .state_set = fake_state_set,
        .state_delete = fake_state_delete,
        .queue_push = fake_queue_push,
        .queue_clear = fake_queue_clear,
        .queue_depth = fake_queue_depth,
        .ctx = storage
    };
    *transport = (honch_transport_ops_t) {
        .post_chunk = fake_post_chunk,
        .ctx = storage
    };
    return (honch_core_config_t) {
        .api_key = "test-key",
        .endpoint_url = "http://collector.local",
        .device_model = "model-x",
        .firmware_version = "1.0.0",
        .environment = "test",
        .queue_directory = "fake",
        .disable_background_flush = 1,
        .durability_mode = HONCH_DURABILITY_OS_BUFFERED,
        .platform = platform,
        .storage = storage_ops,
        .transport = transport
    };
}

static void test_core_state_lock_works_without_platform_lock_callbacks(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "custom_event", NULL) == HONCH_OK);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_failed_firmware_update_queue_does_not_advance_persisted_version(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_ERROR_IO};
    snprintf(storage.firmware_version, sizeof(storage.firmware_version), "1.0.0");
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);
    config.firmware_version = "1.1.0";

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_IO);
    assert(client == NULL);
    assert(strcmp(storage.firmware_version, "1.0.0") == 0);
}

static void test_failed_reset_second_identity_write_preserves_persisted_identity(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    snprintf(storage.device_id, sizeof(storage.device_id), "device-old");
    snprintf(storage.distinct_id, sizeof(storage.distinct_id), "device-old");
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    storage.fail_distinct_id_set = 1;
    assert(honch_core_reset(client) == HONCH_ERROR_IO);
    assert(strcmp(storage.device_id, "device-old") == 0);
    assert(strcmp(storage.distinct_id, "device-old") == 0);
    storage.fail_distinct_id_set = 0;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_state_get_rejects_size_overflow(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .state_size_fault_key = "device_id"
    };
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(client == NULL);
}

static void test_state_get_rejects_inconsistent_read_size(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .state_read_overreport_key = "device_id"
    };
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(client == NULL);
}

static void test_battery_callback_runs_outside_client_mutex(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);
    config.battery_callback = lock_observing_battery_callback;

    g_callback_lock_client = NULL;
    g_battery_callback_saw_unlocked_mutex = 0;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    g_callback_lock_client = client;
    assert(honch_core_track(client, "battery_lock_probe", NULL) == HONCH_OK);
    assert(g_battery_callback_saw_unlocked_mutex == 1);
    g_callback_lock_client = NULL;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_auto_properties_callback_runs_outside_client_mutex(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &storage_ops, &transport);
    config.auto_properties_callback = lock_observing_auto_properties_callback;

    g_callback_lock_client = NULL;
    g_auto_properties_callback_saw_unlocked_mutex = 0;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    g_callback_lock_client = client;
    assert(honch_core_track(client, "auto_properties_lock_probe", NULL) == HONCH_OK);
    assert(g_auto_properties_callback_saw_unlocked_mutex == 1);
    g_callback_lock_client = NULL;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

int main(void)
{
    test_core_state_lock_works_without_platform_lock_callbacks();
    test_failed_firmware_update_queue_does_not_advance_persisted_version();
    test_failed_reset_second_identity_write_preserves_persisted_identity();
    test_state_get_rejects_size_overflow();
    test_state_get_rejects_inconsistent_read_size();
    test_battery_callback_runs_outside_client_mutex();
    test_auto_properties_callback_runs_outside_client_mutex();
    return 0;
}
