#include "honch_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_state_storage {
    char device_id[64];
    char distinct_id[64];
    char firmware_version[64];
    size_t queue_depth;
    uint64_t now_ms;
    uint64_t uptime_ms;
    int force_now_ms;
    honch_status_t queue_push_status;
    uint64_t queued_sequences[8];
    uint8_t last_queued_data[512];
    uint8_t last_queued_prefix[4];
    size_t last_queued_size;
    const uint8_t *read_batch_data;
    size_t read_batch_size;
    size_t queued_sequence_count;
    int queue_push_calls;
    int queue_depth_calls;
    int fail_queue_push_call;
    int queue_consume_calls;
    int queue_drop_oldest_calls;
    int queue_peek_calls;
    int track_queue_depth;
    const char *state_size_fault_key;
    const char *state_read_overreport_key;
    int fail_distinct_id_set;
    honch_client_t *nested_flush_client;
    honch_status_t nested_flush_status;
    int nested_flush_attempts;
    honch_status_t post_chunk_status;
    honch_transport_result_t post_chunk_result;
    int post_chunk_result_from_frame_flags;
    int post_chunk_calls;
    uint64_t retry_after_ms;
    int retry_after_calls;
    int force_connectivity;
    int connectivity_available;
    int connectivity_calls;
    uint8_t coredump_blob[1300];
    size_t coredump_blob_size;
    int coredump_clear_calls;
    int coredump_frame_count;
    int coredump_final_frames;
    int coredump_read_fail_at;     /* fail the Nth coredump read once, then resume */
    int coredump_read_calls;
    int coredump_force_retry;      /* return (HONCH_OK, RETRY) for coredump frames */
    int nested_flush_on_coredump;  /* trigger the re-entrant flush from a coredump post */
} fake_state_storage_t;

static honch_core_config_t fake_config(
    fake_state_storage_t *storage,
    honch_platform_ops_t *platform,
    honch_state_storage_ops_t *state_ops,
    honch_event_queue_ops_t *queue_ops,
    honch_transport_ops_t *transport);

static uint64_t fake_now_ms(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage != NULL && storage->force_now_ms) {
        return storage->now_ms;
    }
    return 1000u;
}

static uint64_t fake_uptime_ms(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage != NULL && storage->uptime_ms > 0u) {
        return storage->uptime_ms;
    }
    return fake_now_ms(ctx);
}

static honch_status_t fake_random_bytes(void *ctx, uint8_t *buffer, size_t buffer_size)
{
    (void)ctx;
    for (size_t i = 0u; i < buffer_size; i++) {
        buffer[i] = (uint8_t)(0xa0u + i);
    }
    return HONCH_OK;
}

static const honch_wire_v2_property_t *find_record_property(
    const honch_event_record_t *record,
    const char *key)
{
    for (size_t i = 0u; i < record->property_count; i++) {
        if (strcmp(record->properties[i].key, key) == 0) {
            return &record->properties[i];
        }
    }
    return NULL;
}

static void assert_record_string_property(
    const honch_event_record_t *record,
    const char *key,
    const char *expected)
{
    const honch_wire_v2_property_t *property = find_record_property(record, key);
    assert(property != NULL);
    assert(property->value.type == HONCH_WIRE_V2_VALUE_TYPE_STRING);
    assert(property->value.string_value != NULL);
    assert(strcmp(property->value.string_value, expected) == 0);
}

static char *fake_state_value(fake_state_storage_t *storage, const char *key)
{
    /* NVS — a primary state-storage backend on ESP32 — caps keys at 15 chars
     * (NVS_KEY_NAME_MAX_SIZE = 16 incl. the NUL). The SDK must not use a longer
     * state key on any port, or persistence silently fails (KEY_TOO_LONG). */
    assert(strlen(key) <= 15u && "state-storage key exceeds NVS 15-char limit");
    if (strcmp(key, "device_id") == 0) {
        return storage->device_id;
    }
    if (strcmp(key, "distinct_id") == 0) {
        return storage->distinct_id;
    }
    if (strcmp(key, "fw_version") == 0) {
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
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_push_calls++;
    if (storage->queue_push_status != HONCH_OK) {
        return storage->queue_push_status;
    }
    if (storage->fail_queue_push_call > 0 && storage->queue_push_calls == storage->fail_queue_push_call) {
        return HONCH_ERROR_IO;
    }
    if (storage->queued_sequence_count >= sizeof(storage->queued_sequences) / sizeof(storage->queued_sequences[0])) {
        return HONCH_ERROR_QUEUE_FULL;
    }
    storage->last_queued_size = event_size;
    memset(storage->last_queued_prefix, 0, sizeof(storage->last_queued_prefix));
    memset(storage->last_queued_data, 0, sizeof(storage->last_queued_data));
    if (event != NULL && event_size >= sizeof(storage->last_queued_prefix)) {
        memcpy(storage->last_queued_prefix, event, sizeof(storage->last_queued_prefix));
    }
    if (event != NULL && event_size <= sizeof(storage->last_queued_data)) {
        memcpy(storage->last_queued_data, event, event_size);
    }
    storage->queued_sequences[storage->queued_sequence_count++] = sequence;
    if (storage->track_queue_depth) {
        storage->queue_depth = storage->queued_sequence_count;
    }
    return HONCH_OK;
}

static honch_status_t fake_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage == NULL || reader == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if ((size_t)storage->queue_peek_calls >= storage->queued_sequence_count) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    *reader = (honch_storage_reader_t) {
        .sequence = storage->queued_sequences[storage->queue_peek_calls++]
    };
    return HONCH_OK;
}

static void test_queue_push_uses_honch_event_record_format(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.last_queued_size > 4u);
    assert(memcmp(storage.last_queued_prefix, "HQR1", 4u) == 0);
    assert(storage.last_queued_prefix[0] != 0xa4u);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_zero_platform_time_queues_parseable_event_record(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .now_ms = 0u, .force_now_ms = 1};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "early_event", NULL, 0u) == HONCH_OK);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(record.timestamp_ms > 0u);
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_pre_wall_clock_track_uses_uptime_timestamp(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .now_ms = 1000u,
        .uptime_ms = 123456u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "early_event", NULL, 0u) == HONCH_OK);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(record.timestamp_ms == 123456u);
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_core_applies_embedded_defaults_when_tuning_is_omitted(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(client->max_event_bytes == 8192u);
    assert(client->transport_timeout_ms == 2500u);
    assert(client->flush_interval_seconds == 120u);
    assert(client->flush_min_interval_ms == 15000u);
    assert(client->flush_event_threshold == 20u);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_normal_reset_emits_only_device_boot(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_NONE,
        .severity = HONCH_CRASH_SEVERITY_INFO,
        .reset_reason = "power_on"
    };
    config.crash_report = &crash;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_push_calls == 1);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$device_boot") == 0);
    assert_record_string_property(&record, "reset_reason", "power_on");
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

#if HONCH_ENABLE_CRASH_CAPTURE
static int g_crash_uploaded_calls = 0;

static void count_crash_uploaded(void *userdata)
{
    (void)userdata;
    g_crash_uploaded_calls++;
}

static void test_abnormal_reset_emits_crash_event(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic",
        .message = "abort",
        .component = "app",
        .exception_cause = "StoreProhibited",
        .fault_pc = "0x400d1a2c",
        .backtrace = "0x400d1a2c,0x40081234",
        .firmware_build_id = "abc123",
        .summary_version = 1u,
        .coredump_available = 1
    };
    config.crash_report = &crash;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_push_calls == 2);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$crash") == 0);
    assert_record_string_property(&record, "source", "panic");
    assert_record_string_property(&record, "severity", "fatal");
    assert_record_string_property(&record, "reset_reason", "panic");
    assert_record_string_property(&record, "message", "abort");
    assert_record_string_property(&record, "component", "app");
    assert_record_string_property(&record, "exception_cause", "StoreProhibited");
    assert_record_string_property(&record, "fault_pc", "0x400d1a2c");
    assert_record_string_property(&record, "backtrace", "0x400d1a2c,0x40081234");
    assert_record_string_property(&record, "firmware_build_id", "abc123");
    const honch_wire_v2_property_t *summary_version = find_record_property(&record, "summary_version");
    assert(summary_version != NULL);
    assert(summary_version->value.type == HONCH_WIRE_V2_VALUE_TYPE_UINT);
    assert(summary_version->value.uint_value == 1u);
    const honch_wire_v2_property_t *coredump = find_record_property(&record, "coredump_available");
    assert(coredump != NULL);
    assert(coredump->value.type == HONCH_WIRE_V2_VALUE_TYPE_BOOL);
    assert(coredump->value.bool_value);
    /* A crash_id linking this summary to its coredump blob is always present. */
    const honch_wire_v2_property_t *crash_id = find_record_property(&record, "crash_id");
    assert(crash_id != NULL);
    assert(crash_id->value.type == HONCH_WIRE_V2_VALUE_TYPE_STRING);
    assert(crash_id->value.string_value != NULL && crash_id->value.string_value[0] != '\0');
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_oversized_crash_event_is_skipped_without_failing_init(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_WATCHDOG,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "task_wdt"
    };
    config.crash_report = &crash;
    config.max_event_bytes = 100u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_push_calls == 1);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$device_boot") == 0);
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_overlong_crash_fields_are_omitted_without_large_allocation(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    char long_message[900];
    memset(long_message, 'x', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_WATCHDOG,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "task_wdt",
        .message = long_message,
        .component = "rtos"
    };
    config.crash_report = &crash;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_push_calls == 2);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$crash") == 0);
    assert_record_string_property(&record, "source", "watchdog");
    assert_record_string_property(&record, "reset_reason", "task_wdt");
    assert(find_record_property(&record, "message") == NULL);
    assert_record_string_property(&record, "component", "rtos");
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_report_crash_emits_crash_event(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int boot_pushes = storage.queue_push_calls;

    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_EXCEPTION,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "exception",
        .message = "camera worker crashed",
        .component = "camera"
    };
    assert(honch_core_report_crash(client, &crash) == HONCH_OK);
    assert(storage.queue_push_calls == boot_pushes + 1);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$crash") == 0);
    assert_record_string_property(&record, "source", "exception");
    assert_record_string_property(&record, "message", "camera worker crashed");
    assert_record_string_property(&record, "component", "camera");
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_report_crash_rejects_null(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_report_crash(client, NULL) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_report_crash_is_once_only(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic"
    };
    assert(honch_core_report_crash(client, &crash) == HONCH_OK);
    int pushes_after_first = storage.queue_push_calls;
    assert(honch_core_report_crash(client, &crash) == HONCH_OK);
    assert(storage.queue_push_calls == pushes_after_first);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_crash_uploaded_callback_fires_after_delivery(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    g_crash_uploaded_calls = 0;
    config.crash_uploaded_callback = count_crash_uploaded;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic"
    };
    assert(honch_core_report_crash(client, &crash) == HONCH_OK);
    assert(g_crash_uploaded_calls == 0);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(g_crash_uploaded_calls == 1);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(g_crash_uploaded_calls == 1);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Regression for the erase-after-ack data-loss window: when the $crash sits
 * behind other events and only one batch is delivered per flush, the callback
 * must fire only on the flush that actually delivers the $crash, not on an
 * earlier flush that uploaded something else. */
static void test_crash_uploaded_callback_waits_for_crash_event_delivery(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    g_crash_uploaded_calls = 0;
    config.crash_uploaded_callback = count_crash_uploaded;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    /* One event per batch, no outbound spacing, so the queued $device_boot is
     * delivered first and $crash only on the second flush. */
    client->batch_size = 1u;
    client->flush_min_interval_ms = 0u;

    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic"
    };
    assert(honch_core_report_crash(client, &crash) == HONCH_OK);
    assert(storage.queued_sequence_count == 2u); /* $device_boot + $crash */

    /* Flush 1 delivers $device_boot only — the coredump must NOT be erased yet. */
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.queued_sequence_count == 1u);
    assert(g_crash_uploaded_calls == 0);

    /* Flush 2 delivers $crash — now the callback fires exactly once. */
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.queued_sequence_count == 0u);
    assert(g_crash_uploaded_calls == 1);

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static size_t fake_coredump_size(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    return storage->coredump_blob_size;
}

static int fake_coredump_read(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->coredump_read_calls++;
    if (storage->coredump_read_fail_at != 0 &&
        storage->coredump_read_calls == storage->coredump_read_fail_at) {
        storage->coredump_read_fail_at = 0; /* fail once, then let the resume read succeed */
        return -1;
    }
    if (offset + len > storage->coredump_blob_size) {
        return -1;
    }
    memcpy(out, storage->coredump_blob + offset, len);
    return (int)len;
}

static void fake_coredump_clear(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->coredump_clear_calls++;
    storage->coredump_blob_size = 0u; /* simulate the flash erase: size() is now 0 */
}

static void test_coredump_streams_and_erases_after_full_delivery(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    storage.coredump_blob_size = sizeof(storage.coredump_blob); /* 1300 bytes */
    storage.post_chunk_result_from_frame_flags = 1; /* conforming server: CHUNK_STORED then ACCEPTED */
    for (size_t i = 0u; i < sizeof(storage.coredump_blob); i++) {
        storage.coredump_blob[i] = (uint8_t)(i * 7u + 3u);
    }
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic",
        .coredump_available = 1
    };
    config.crash_report = &crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size,
        .read = fake_coredump_read,
        .clear = fake_coredump_clear,
        .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u; /* disable outbound spacing for back-to-back flushes */

    /* 1300 bytes / 512-byte chunks = 3 frames (512, 512, 276); the 3rd is final. */
    for (int i = 0; i < 12 && storage.coredump_clear_calls == 0; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    assert(storage.coredump_frame_count == 3);
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 1); /* erase-after-ack, exactly once */

    /* After the erase the source is empty, so no further coredump frames are sent. */
    int frames_after_clear = storage.coredump_frame_count;
    storage.now_ms += 1u;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.coredump_frame_count == frames_after_clear);
    assert(storage.coredump_clear_calls == 1);

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Fill a storage's coredump blob with deterministic bytes. */
static void seed_coredump_blob(fake_state_storage_t *storage)
{
    storage->coredump_blob_size = sizeof(storage->coredump_blob); /* 1300 bytes */
    for (size_t i = 0u; i < sizeof(storage->coredump_blob); i++) {
        storage->coredump_blob[i] = (uint8_t)(i * 7u + 3u);
    }
}

static honch_crash_report_t g_coredump_crash = {
    .kind = HONCH_CRASH_KIND_PANIC,
    .severity = HONCH_CRASH_SEVERITY_FATAL,
    .reset_reason = "panic",
    .coredump_available = 1
};

/* Critical regression: completion must be LATCHED in client state, not inferred
 * from size()==0. With clear() == NULL the image is never erased, so size() stays
 * non-zero forever — the blob must still stream exactly once and never re-upload. */
static void test_coredump_completion_latched_when_clear_is_null(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = NULL, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u;

    for (int i = 0; i < 20; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    assert(storage.coredump_frame_count == 3);  /* 512 + 512 + 276, streamed ONCE */
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 0);  /* clear was NULL */
    assert(client->coredump_done);              /* terminal latch set */
    assert(storage.coredump_blob_size == sizeof(storage.coredump_blob)); /* image still present */

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Critical regression: when a raw coredump source is wired, the $crash summary's
 * erase-after-ack callback must be SUPPRESSED — it is delivered first and would
 * wipe the partition out from under the in-flight blob (double-erase). The blob's
 * clear() is the single erase, fired only after the blob's final frame is acked. */
static void test_coredump_source_suppresses_summary_erase(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    g_crash_uploaded_calls = 0;
    config.crash_uploaded_callback = count_crash_uploaded; /* BOTH erase paths wired */
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u;

    for (int i = 0; i < 12 && storage.coredump_clear_calls == 0; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    /* The blob completed (final frame sent) — proof it was NOT wiped mid-upload. */
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 1);  /* clear() is the sole erase */
    assert(g_crash_uploaded_calls == 0);         /* summary callback suppressed */

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Major regression: a (HONCH_OK, RETRY) post must NOT advance offset/CRC — the
 * commit decision keys on the transport RESULT, not just status. The chunk is
 * re-sent and the running CRC is never folded over bytes Capture did not store. */
static void test_coredump_retry_result_does_not_advance(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    storage.coredump_force_retry = 1; /* coredump frames return (HONCH_OK, RETRY) */
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u;

    for (int i = 0; i < 5; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    assert(storage.coredump_frame_count >= 2);   /* same chunk re-sent each drive */
    assert(storage.coredump_final_frames == 0);  /* never completes under RETRY */
    assert(client->coredump_offset == 0u);       /* offset frozen */
    assert(client->coredump_committed_crc == HONCH_WIRE_V2_CRC16_INITIAL); /* CRC frozen */
    assert(!client->coredump_done);
    assert(storage.coredump_clear_calls == 0);

    /* Once the transport accepts, the upload completes cleanly and erases. */
    storage.coredump_force_retry = 0;
    for (int i = 0; i < 12 && storage.coredump_clear_calls == 0; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 1);

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Minor regression: a transient read error must RESUME at the committed offset
 * under the same (message_id, total), not restart the whole stream from zero. */
static void test_coredump_resumes_after_transient_read_error(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK, .track_queue_depth = 1};
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    storage.coredump_read_fail_at = 2; /* fail the 2nd read once, then recover */
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u;

    for (int i = 0; i < 12 && storage.coredump_clear_calls == 0; i++) {
        storage.now_ms += 1u;
        assert(honch_core_flush(client) == HONCH_OK);
    }
    /* The mid-stream read failure cost a drive but did NOT restart from offset 0:
     * exactly 3 frames are sent (no duplicate init frame) and the CRC is correct. */
    assert(storage.coredump_frame_count == 3);
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 1);

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Major regression: a transport that re-enters flush from inside the coredump
 * post must get HONCH_ERROR_BUSY (the flush busy-guard is held across the step),
 * not deadlock on the non-recursive state mutex (the lock is released around the
 * post, so the re-entrant call can acquire it and observe flush_in_progress). */
static void test_nested_flush_during_coredump_post_returns_busy(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK, .track_queue_depth = 1, .nested_flush_status = HONCH_OK
    };
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 0u;
    storage.nested_flush_on_coredump = 1;
    storage.nested_flush_client = client;

    (void)honch_core_flush(client); /* must return, not hang */
    assert(storage.nested_flush_attempts == 1);
    assert(storage.nested_flush_status == HONCH_ERROR_BUSY);
    storage.nested_flush_client = NULL;

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Major regression: coredump chunks must respect outbound spacing — once events
 * drain, at most one chunk posts per flush_min_interval_ms window (it can't
 * saturate the link). */
static void test_coredump_respects_outbound_spacing(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK, .track_queue_depth = 1, .force_now_ms = 1
    };
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    client->flush_min_interval_ms = 5000u;
    storage.now_ms = 1000u;

    /* Flush 1: delivers events + the first coredump chunk, opening the window. */
    assert(honch_core_flush(client) == HONCH_OK);
    int after_first = storage.coredump_frame_count;
    assert(after_first >= 1);

    /* Flush 2 inside the same window is rate-limited — no extra coredump chunk. */
    assert(honch_core_flush(client) == HONCH_ERROR_RATE_LIMITED);
    assert(storage.coredump_frame_count == after_first);

    /* Past the window: exactly one more chunk. */
    storage.now_ms += 5000u;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.coredump_frame_count == after_first + 1);

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* Major regression (re-review F5): tick(), not flush(), is the production driver
 * on ESP/Arduino. A coredump must advance at the outbound-spacing cadence on
 * tick() even with a drained event queue. Before the scheduler's due-gate and
 * connectivity-gate learned about an in-flight coredump, tick() only re-drove on
 * the much longer flush_interval (default 120s/chunk), so a multi-KB blob took
 * hours and — with RAM-only offset/CRC — never finished on a device that reboots.
 * Here a 3-frame blob must complete in a handful of 1s spacing windows, NOT the
 * 3 * 120s = 360 ticks the flush_interval timer alone would need. */
static void test_coredump_tick_advances_at_spacing_cadence(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK, .track_queue_depth = 1, .force_now_ms = 1
    };
    seed_coredump_blob(&storage);
    storage.post_chunk_result_from_frame_flags = 1;
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.crash_report = &g_coredump_crash;
    honch_coredump_source_t source = {
        .size = fake_coredump_size, .read = fake_coredump_read, .clear = fake_coredump_clear, .ctx = &storage
    };
    config.coredump_source = &source;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(client->flush_interval_seconds == 120u); /* the slow timer that used to gate the blob */
    client->flush_min_interval_ms = 1000u;          /* 1s spacing window */
    storage.now_ms = 1000u;

    /* One flush delivers the queued events and posts the first coredump chunk
     * (upload now in flight, event queue drained) — the device then idles on
     * tick() alone. */
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.coredump_frame_count >= 1);
    assert(storage.coredump_clear_calls == 0);

    /* Drive ONLY via tick(), advancing 1s per tick. Total elapsed stays far below
     * the 120000ms flush_interval, so if the blob finishes it can ONLY be the
     * coredump-due gate driving it — not the interval timer. */
    int ticks = 0;
    for (; ticks < 20 && storage.coredump_clear_calls == 0; ticks++) {
        storage.now_ms += 1000u; /* one spacing window per tick */
        (void)honch_core_tick(client);
    }
    assert(storage.coredump_final_frames == 1);
    assert(storage.coredump_clear_calls == 1);
    assert(storage.coredump_frame_count == 3);
    assert(ticks < 10); /* a few 1s windows, not the 120s timer (which never fired here) */

    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}
#endif /* HONCH_ENABLE_CRASH_CAPTURE */

#if HONCH_ENABLE_LOG_CAPTURE
static void test_log_error_emits_error_event(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_report_log_error(client, "wifi", "connect failed") == HONCH_OK);
    int pushes_before_flush = storage.queue_push_calls;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.queue_push_calls == pushes_before_flush + 1);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$error") == 0);
    assert_record_string_property(&record, "level", "error");
    assert_record_string_property(&record, "component", "wifi");
    assert_record_string_property(&record, "message", "connect failed");
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_identical_log_errors_coalesce(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    for (int i = 0; i < 20; i++) {
        assert(honch_core_report_log_error(client, "wifi", "connect failed") == HONCH_OK);
    }
    int pushes_before_flush = storage.queue_push_calls;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.queue_push_calls == pushes_before_flush + 1);

    honch_event_record_t record;
    assert(honch_event_record_parse(storage.last_queued_data, storage.last_queued_size, &record) == HONCH_OK);
    assert(strcmp(record.event_name, "$error") == 0);
    const honch_wire_v2_property_t *count = find_record_property(&record, "count");
    assert(count != NULL);
    assert(count->value.type == HONCH_WIRE_V2_VALUE_TYPE_UINT);
    assert(count->value.uint_value == 20u);
    honch_event_record_free(&record);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_sdk_self_logs_are_ignored(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int pushes_before = storage.queue_push_calls;
    assert(honch_core_report_log_error(client, "honch", "internal detail") == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.queue_push_calls == pushes_before);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

/* A clean shutdown must drain coalesced log errors that accumulated since the
 * last flush/tick, not silently drop them. */
static void test_shutdown_drains_pending_log_errors(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_report_log_error(client, "sensor", "read timeout") == HONCH_OK);
    int pushes_before_shutdown = storage.queue_push_calls;
    assert(honch_core_shutdown(client) == HONCH_OK);
    /* Shutdown must enqueue both the drained $error and $device_shutdown (+2),
     * not just $device_shutdown (+1). */
    assert(storage.queue_push_calls == pushes_before_shutdown + 2);
}
#endif /* HONCH_ENABLE_LOG_CAPTURE */

static honch_status_t fake_queue_consume(void *ctx, uint64_t sequence)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_consume_calls++;
    for (size_t i = 0u; i < storage->queued_sequence_count; i++) {
        if (storage->queued_sequences[i] != sequence) {
            continue;
        }
        for (size_t j = i + 1u; j < storage->queued_sequence_count; j++) {
            storage->queued_sequences[j - 1u] = storage->queued_sequences[j];
        }
        storage->queued_sequence_count--;
        if (storage->track_queue_depth) {
            storage->queue_depth = storage->queued_sequence_count;
        }
        return HONCH_OK;
    }
    return HONCH_ERROR_NOT_INITIALIZED;
}

static honch_status_t fake_queue_clear(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_depth = 0u;
    storage->queued_sequence_count = 0u;
    return HONCH_OK;
}

static honch_status_t fake_queue_drop_oldest(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_drop_oldest_calls++;
    if (storage->queued_sequence_count == 0u) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    for (size_t i = 1u; i < storage->queued_sequence_count; i++) {
        storage->queued_sequences[i - 1u] = storage->queued_sequences[i];
    }
    storage->queued_sequence_count--;
    if (storage->track_queue_depth) {
        storage->queue_depth = storage->queued_sequence_count;
    }
    return HONCH_OK;
}

static honch_status_t fake_queue_depth(void *ctx, size_t *depth)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    storage->queue_depth_calls++;
    *depth = storage->queue_depth;
    return HONCH_OK;
}

static honch_status_t fake_queue_read_batch(
    void *ctx,
    honch_storage_event_t *events,
    size_t batch_size,
    size_t max_event_bytes,
    size_t *event_count)
{
    (void)max_event_bytes;
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage == NULL || events == NULL || event_count == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *event_count = 0u;
    size_t count = storage->queued_sequence_count < batch_size ? storage->queued_sequence_count : batch_size;
    const uint8_t *source_data = storage->read_batch_data != NULL ? storage->read_batch_data : storage->last_queued_data;
    size_t source_size = storage->read_batch_data != NULL ? storage->read_batch_size : storage->last_queued_size;
    for (size_t i = 0u; i < count; i++) {
        uint8_t *copy = (uint8_t *)malloc(source_size);
        if (copy == NULL) {
            for (size_t j = 0u; j < i; j++) {
                free(events[j].data);
                events[j].data = NULL;
            }
            *event_count = 0u;
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        memcpy(copy, source_data, source_size);
        events[i] = (honch_storage_event_t) {
            .sequence = storage->queued_sequences[i],
            .data = copy,
            .length = source_size
        };
        (*event_count)++;
    }
    return count == 0u ? HONCH_ERROR_NOT_INITIALIZED : HONCH_OK;
}

static void build_heavy_event(
    const char *event_name,
    size_t property_count,
    size_t array_length,
    size_t final_array_length,
    uint8_t **out_data,
    size_t *out_size)
{
    assert(out_data != NULL);
    assert(out_size != NULL);

    honch_wire_v2_property_t *properties = (honch_wire_v2_property_t *)calloc(property_count, sizeof(*properties));
    assert(properties != NULL);
    for (size_t i = 0u; i < property_count; i++) {
        char key[8];
        int key_length = snprintf(key, sizeof(key), "p%02zu", i);
        assert(key_length > 0 && (size_t)key_length < sizeof(key));
        char *stored_key = honch_strdup(key);
        size_t item_count = i + 1u == property_count ? final_array_length : array_length;
        honch_wire_v2_value_t *items = (honch_wire_v2_value_t *)calloc(item_count, sizeof(*items));
        assert(stored_key != NULL);
        assert(items != NULL);
        for (size_t j = 0u; j < item_count; j++) {
            items[j] = honch_u64((uint64_t)(i + j));
        }
        properties[i] = honch_prop(stored_key, honch_array(items, item_count));
    }

    honch_payload_t payload = {0};
    assert(honch_event_record_build(event_name, "device-1", NULL, 1234u, properties, property_count, &payload) ==
        HONCH_OK);
    for (size_t i = 0u; i < property_count; i++) {
        free((void *)properties[i].key);
        free((void *)properties[i].value.array.items);
    }
    free(properties);
    *out_data = payload.data;
    *out_size = payload.length;
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
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    /* A coredump frame: header bits 2-4 carry source_type (1 = coredump),
     * bit 6 (0x40) is the `more` flag — a final frame has it clear. */
    bool is_coredump = body != NULL && body_size > 0u && ((body[0] >> 2u) & 0x7u) == 1u;
    if (storage != NULL) {
        storage->post_chunk_calls++;
        if (is_coredump) {
            storage->coredump_frame_count++;
            if ((body[0] & 0x40u) == 0u) {
                storage->coredump_final_frames++;
            }
        }
    }
    /* Re-entrancy probe: fire a nested flush from within the post. By default it
     * fires on the first post (an event frame); nested_flush_on_coredump targets
     * a coredump post specifically (the lock is released around it, so the nested
     * call must still hit the flush_in_progress busy-guard, not deadlock). */
    if (storage != NULL && storage->nested_flush_client != NULL && storage->nested_flush_attempts == 0 &&
        (!storage->nested_flush_on_coredump || is_coredump)) {
        storage->nested_flush_attempts++;
        storage->nested_flush_status = honch_core_flush(storage->nested_flush_client);
    }
    if (storage != NULL && is_coredump && storage->coredump_force_retry) {
        /* Non-conforming (HONCH_OK, RETRY) on a coredump frame: the uploader must
         * NOT advance offset/CRC and must re-send the same chunk. */
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_OK;
    }
    if (storage != NULL && storage->post_chunk_result_from_frame_flags) {
        *result = body_size > 0u && (body[0] & 0x40u) != 0u ?
            HONCH_TRANSPORT_CHUNK_STORED :
            HONCH_TRANSPORT_ACCEPTED;
    } else {
        *result = storage != NULL && storage->post_chunk_result != 0 ?
            storage->post_chunk_result :
            HONCH_TRANSPORT_ACCEPTED;
    }
    return storage != NULL && storage->post_chunk_status != 0 ?
        storage->post_chunk_status :
        HONCH_OK;
}

static uint64_t fake_retry_after_ms(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage == NULL) {
        return 0u;
    }
    storage->retry_after_calls++;
    return storage->retry_after_ms;
}

static int fake_connectivity_available(void *ctx)
{
    fake_state_storage_t *storage = (fake_state_storage_t *)ctx;
    if (storage == NULL) {
        return 1;
    }
    storage->connectivity_calls++;
    return storage->force_connectivity ? storage->connectivity_available : 1;
}

static honch_client_t *g_callback_lock_client = NULL;
static int g_battery_callback_saw_unlocked_mutex = 0;
static int g_auto_properties_callback_saw_unlocked_mutex = 0;
static honch_client_t *g_recursive_auto_client = NULL;
static int g_recursive_auto_depth = 0;
static int g_recursive_auto_max_depth = 0;

static int lock_observing_battery_callback(void)
{
    if (g_callback_lock_client != NULL) {
        g_battery_callback_saw_unlocked_mutex = 1;
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
        g_auto_properties_callback_saw_unlocked_mutex = 1;
    }

    return sink(sink_ctx, "$wifi_rssi", honch_i64(-42));
}

static honch_status_t recursive_auto_properties_callback(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    (void)userdata;
    g_recursive_auto_depth++;
    if (g_recursive_auto_depth < g_recursive_auto_max_depth) {
        char event_name[32];
        snprintf(event_name, sizeof(event_name), "recursive_auto_%d", g_recursive_auto_depth);
        honch_status_t nested_status = honch_core_track(g_recursive_auto_client, event_name, NULL, 0u);
        if (nested_status != HONCH_OK) {
            g_recursive_auto_depth--;
            return nested_status;
        }
    }

    honch_status_t status = sink(sink_ctx, "$wifi_rssi", honch_i64(-40 - g_recursive_auto_depth));
    g_recursive_auto_depth--;
    return status;
}

static honch_core_config_t fake_config(
    fake_state_storage_t *storage,
    honch_platform_ops_t *platform,
    honch_state_storage_ops_t *state_ops,
    honch_event_queue_ops_t *queue_ops,
    honch_transport_ops_t *transport)
{
    *platform = (honch_platform_ops_t) {
        .now_ms = fake_now_ms,
        .uptime_ms = fake_uptime_ms,
        .random_bytes = fake_random_bytes,
        .ctx = storage
    };
    *state_ops = (honch_state_storage_ops_t) {
        .state_get = fake_state_get,
        .state_set = fake_state_set,
        .state_delete = fake_state_delete,
        .ctx = storage
    };
    *queue_ops = (honch_event_queue_ops_t) {
        .queue_push = fake_queue_push,
        .queue_peek = fake_queue_peek,
        .queue_consume = fake_queue_consume,
        .queue_drop_oldest = fake_queue_drop_oldest,
        .queue_clear = fake_queue_clear,
        .queue_depth = fake_queue_depth,
        .queue_read_batch = fake_queue_read_batch,
        .ctx = storage
    };
    *transport = (honch_transport_ops_t) {
        .post_chunk = fake_post_chunk,
        .retry_after_ms = fake_retry_after_ms,
        .ctx = storage
    };
    return (honch_core_config_t) {
        .api_key = "test-key",
        .endpoint_url = "http://collector.local",
        .device_model = "model-x",
        .firmware_version = "1.0.0",
        .environment = "test",
        .queue_directory = "fake",
        .durability_mode = HONCH_DURABILITY_OS_BUFFERED,
        .connectivity_callback = fake_connectivity_available,
        .connectivity_userdata = storage,
        .platform = platform,
        .state_storage = state_ops,
        .event_queue = queue_ops,
        .transport = transport
    };
}

static void test_core_state_lock_works_without_platform_lock_callbacks(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "custom_event", NULL, 0u) == HONCH_OK);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_init_rejects_mutex_required_platform_without_lock_callbacks(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    platform.requires_mutex = true;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(client == NULL);
}

static void test_failed_firmware_update_queue_does_not_advance_persisted_version(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_ERROR_IO};
    snprintf(storage.firmware_version, sizeof(storage.firmware_version), "1.0.0");
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.firmware_version = "1.1.0";

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_IO);
    assert(client == NULL);
    assert(strcmp(storage.firmware_version, "1.0.0") == 0);
}

static void test_failed_init_rolls_back_queued_lifecycle_events(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .fail_queue_push_call = 2,
        .track_queue_depth = 1
    };
    snprintf(storage.firmware_version, sizeof(storage.firmware_version), "1.0.0");
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.firmware_version = "1.1.0";

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_IO);
    assert(client == NULL);
    assert(storage.queue_push_calls == 2);
    assert(storage.queue_consume_calls == 1);
    assert(storage.queue_depth == 0u);
    assert(storage.queued_sequence_count == 0u);
    assert(strcmp(storage.firmware_version, "1.0.0") == 0);
}

#if HONCH_ENABLE_CRASH_CAPTURE
static void test_failed_crash_capture_queue_rolls_back_lifecycle_events(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .fail_queue_push_call = 2,
        .track_queue_depth = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    honch_crash_report_t crash = {
        .kind = HONCH_CRASH_KIND_PANIC,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = "panic"
    };
    config.crash_report = &crash;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_IO);
    assert(client == NULL);
    assert(storage.queue_push_calls == 2);
    assert(storage.queue_consume_calls == 1);
    assert(storage.queue_depth == 0u);
    assert(storage.queued_sequence_count == 0u);
}
#endif

static void test_failed_reset_second_identity_write_preserves_persisted_identity(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    snprintf(storage.device_id, sizeof(storage.device_id), "device-old");
    snprintf(storage.distinct_id, sizeof(storage.distinct_id), "device-old");
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    storage.fail_distinct_id_set = 1;
    assert(honch_core_reset(client) == HONCH_ERROR_IO);
    assert(strcmp(storage.device_id, "device-old") == 0);
    assert(strcmp(storage.distinct_id, "device-old") == 0);
    storage.fail_distinct_id_set = 0;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_set_property_rejects_blank_key(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    assert(honch_core_set_property(client, "", honch_str("value")) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_core_set_property(client, "   ", honch_str("value")) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(storage.queue_push_calls == queue_push_calls);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_set_property_rejects_reserved_key(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    assert(honch_core_set_property(client, "$device_id", honch_str("spoof")) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_core_set_property(client, "$session_id", honch_str("spoof")) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(storage.queue_push_calls == queue_push_calls);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_track_rejects_promoted_distinct_id_property_key(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    const honch_wire_v2_property_t properties[] = {
        honch_prop("distinct_id", honch_str("spoofed"))
    };
    assert(honch_core_track(client, "bad_distinct_id_property", properties, 1u) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(storage.queue_push_calls == queue_push_calls);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_init_sets_next_sequence_after_existing_storage_events(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .queue_depth = 3u,
        .queued_sequences = {2u, 9u, 4u},
        .queued_sequence_count = 3u
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_peek_calls == 3);
    assert(storage.queued_sequence_count == 4u);
    assert(storage.queued_sequences[3] == 10u);
    assert(honch_core_track(client, "after_existing_storage", NULL, 0u) == HONCH_OK);
    assert(storage.queued_sequence_count == 5u);
    assert(storage.queued_sequences[4] == 11u);

    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_sequence_wrap_rejects_enqueue_without_advancing(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    client->sequence = UINT64_MAX;
    assert(honch_core_track(client, "sequence_wrap_probe", NULL, 0u) == HONCH_ERROR_QUEUE_FULL);
    assert(client->sequence == UINT64_MAX);
    assert(storage.queue_push_calls == queue_push_calls);
    client->sequence = 42u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_custom_storage_enqueue_updates_cached_queue_depth_without_refresh(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_depth == 1u);
    assert(client->queued_event_count == storage.queue_depth);
    int queue_depth_calls = storage.queue_depth_calls;
    assert(honch_core_track(client, "custom_depth_probe", NULL, 0u) == HONCH_OK);
    assert(storage.queue_depth_calls == queue_depth_calls);
    assert(storage.queue_depth == 2u);
    assert(client->queued_event_count == storage.queue_depth);
    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_custom_storage_drops_oldest_at_queue_limit(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.max_queued_events = 1u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(storage.queue_depth == 1u);
    assert(honch_core_track(client, "bounded_custom_queue", NULL, 0u) == HONCH_OK);
    assert(storage.queue_drop_oldest_calls == 1);
    assert(storage.queue_depth == 1u);
    assert(client->queued_event_count == 1u);
    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_failed_session_replacement_preserves_old_session(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_session_start(client, "old") == HONCH_OK);
    assert(client->session_id != NULL);
    char old_session_id[64];
    snprintf(old_session_id, sizeof(old_session_id), "%s", client->session_id);
    size_t depth_before_replacement = storage.queue_depth;

    storage.fail_queue_push_call = storage.queue_push_calls + 2;
    assert(honch_core_session_start(client, "replacement") == HONCH_ERROR_IO);
    assert(client->session_id != NULL);
    assert(strcmp(client->session_id, old_session_id) == 0);
    assert(storage.queue_depth == depth_before_replacement);

    storage.fail_queue_push_call = 0;
    storage.track_queue_depth = 0;
    storage.queue_depth = 0u;
    storage.queued_sequence_count = 0u;
    client->queued_event_count = 0u;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_track_rejects_embedded_nul_property_key(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    const char invalid_key[] = {(char)0xff, '\0'};
    const honch_property_t properties[] = {
        honch_prop(invalid_key, honch_i64(1))
    };
    assert(honch_core_track(client, "bad_property_key", properties, 1u) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(storage.queue_push_calls == queue_push_calls);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_identify_rejects_embedded_nul_trait_key(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    int queue_push_calls = storage.queue_push_calls;
    const char invalid_key[] = {(char)0xff, '\0'};
    const honch_property_t traits[] = {
        honch_prop(invalid_key, honch_i64(1))
    };
    assert(honch_core_identify(client, "user-1", traits, 1u) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(strcmp(storage.distinct_id, storage.device_id) == 0);
    assert(storage.queue_push_calls == queue_push_calls);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_state_get_rejects_size_overflow(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .state_size_fault_key = "device_id"
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

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
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(client == NULL);
}

static void test_battery_callback_runs_outside_client_mutex(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.battery_callback = lock_observing_battery_callback;

    g_callback_lock_client = NULL;
    g_battery_callback_saw_unlocked_mutex = 0;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    g_callback_lock_client = client;
    assert(honch_core_track(client, "battery_lock_probe", NULL, 0u) == HONCH_OK);
    assert(g_battery_callback_saw_unlocked_mutex == 1);
    g_callback_lock_client = NULL;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_auto_properties_callback_runs_outside_client_mutex(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.auto_properties_callback = lock_observing_auto_properties_callback;

    g_callback_lock_client = NULL;
    g_auto_properties_callback_saw_unlocked_mutex = 0;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    g_callback_lock_client = client;
    assert(honch_core_track(client, "auto_properties_lock_probe", NULL, 0u) == HONCH_OK);
    assert(g_auto_properties_callback_saw_unlocked_mutex == 1);
    g_callback_lock_client = NULL;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_auto_property_buffer_exhaustion_returns_busy(void)
{
    fake_state_storage_t storage = {.queue_push_status = HONCH_OK};
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.auto_properties_callback = recursive_auto_properties_callback;

    g_recursive_auto_client = NULL;
    g_recursive_auto_depth = 0;
    g_recursive_auto_max_depth = 1;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    g_recursive_auto_client = client;
    g_recursive_auto_max_depth = 8;
    assert(honch_core_track(client, "recursive_auto_root", NULL, 0u) == HONCH_ERROR_BUSY);
    assert(g_recursive_auto_depth == 0);
    g_recursive_auto_client = NULL;
    g_recursive_auto_max_depth = 1;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_nested_flush_during_transport_returns_busy(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .nested_flush_status = HONCH_OK
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "nested_flush_probe", NULL, 0u) == HONCH_OK);
    storage.nested_flush_client = client;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.nested_flush_attempts == 1);
    assert(storage.nested_flush_status == HONCH_ERROR_BUSY);
    storage.nested_flush_client = NULL;
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_retry_after_extends_scheduler_delay(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 10000u,
        .force_now_ms = 1,
        .post_chunk_status = HONCH_ERROR_RATE_LIMITED,
        .post_chunk_result = HONCH_TRANSPORT_RETRY,
        .retry_after_ms = 7000u
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_retry_initial_ms = 1000u;
    config.flush_retry_max_ms = 10000u;
    config.flush_max_batches = 1u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "retry_after_probe", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_ERROR_RATE_LIMITED);
    assert(storage.retry_after_calls == 1);
    assert(client->next_retry_flush_ms == 17000u);
    assert(client->scheduler_flush_requested);
    assert(client->current_retry_delay_ms == 2000u);

    storage.post_chunk_status = HONCH_OK;
    storage.post_chunk_result = HONCH_TRANSPORT_ACCEPTED;
    storage.retry_after_ms = 0u;
    storage.now_ms = 17000u;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(client->next_retry_flush_ms == 0u);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_tick_preserves_requested_flush_during_min_spacing(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 1000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = 10000u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "first_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    assert(honch_core_track(client, "second_flush", NULL, 0u) == HONCH_OK);
    storage.now_ms = 5000u;
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);
    assert(client->scheduler_flush_requested);

    storage.now_ms = 11000u;
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 2);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_tick_posts_at_most_one_chunk_per_call(void)
{
    uint8_t *event = NULL;
    size_t event_size = 0u;
    build_heavy_event("chunked_tick", 64u, 28u, 12u, &event, &event_size);

    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .queue_depth = 1u,
        .queued_sequences = {1u},
        .queued_sequence_count = 1u,
        .read_batch_data = event,
        .read_batch_size = event_size,
        .post_chunk_result_from_frame_flags = 1,
        .now_ms = 1000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    (void)fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);

    honch_client_t client = {0};
    client.platform = &platform;
    client.event_queue = &queue_ops;
    client.transport = &transport;
    client.api_key = "test-key";
    client.endpoint_url = "http://collector.local";
    client.device_id = "device-1";
    client.device_model = "model-x";
    client.firmware_version = "1.0.0";
    client.environment = "test";
    client.sdk_platform = "posix";
    client.batch_size = 1u;
    client.max_event_bytes = 65536u;
    client.flush_event_threshold = 1u;
    client.flush_min_interval_ms = 0u;
    client.flush_retry_initial_ms = HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS;
    client.flush_retry_max_ms = HONCH_DEFAULT_FLUSH_RETRY_MAX_MS;
    client.current_retry_delay_ms = HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS;
    client.scheduler_flush_requested = true;
    client.wire_v2_message_id_seed = 0x100u;
    snprintf(client.wire_v2_stream_id, sizeof(client.wire_v2_stream_id), "boot0001");

    assert(honch_core_tick(&client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);
    assert(storage.queued_sequence_count == 1u);
    assert(storage.queue_consume_calls == 0);

    int ticks = 1;
    while (storage.queued_sequence_count > 0u && ticks < 8) {
        int calls_before = storage.post_chunk_calls;
        storage.now_ms += 1u;
        assert(honch_core_tick(&client) == HONCH_OK);
        ticks++;
        assert(storage.post_chunk_calls == calls_before + 1);
    }

    assert(storage.queued_sequence_count == 0u);
    assert(storage.queue_consume_calls == 1);
    free(event);
}

static void test_flush_returns_rate_limited_during_min_spacing(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 2000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = 10000u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "first_explicit_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    assert(honch_core_track(client, "second_explicit_flush", NULL, 0u) == HONCH_OK);
    storage.now_ms = 6000u;
    assert(honch_core_flush(client) == HONCH_ERROR_RATE_LIMITED);
    assert(storage.post_chunk_calls == 1);
    assert(client->scheduler_flush_requested);

    storage.now_ms = 12000u;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 2);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_empty_flush_is_not_rate_limited_during_min_spacing(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 4000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = 10000u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "only_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    storage.now_ms = 5000u;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);
    assert(!client->scheduler_flush_requested);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_tick_skips_transport_while_connectivity_unavailable(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 10000u,
        .force_now_ms = 1,
        .force_connectivity = 1,
        .connectivity_available = 0
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "offline_tick", NULL, 0u) == HONCH_OK);
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.connectivity_calls == 1);
    assert(storage.post_chunk_calls == 0);
    assert(client->scheduler_flush_requested);

    storage.connectivity_available = 1;
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.connectivity_calls == 2);
    assert(storage.post_chunk_calls == 1);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_flush_returns_offline_without_transport_attempt(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 20000u,
        .force_now_ms = 1,
        .force_connectivity = 1,
        .connectivity_available = 0
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "offline_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_ERROR_OFFLINE);
    assert(storage.connectivity_calls == 1);
    assert(storage.post_chunk_calls == 0);
    assert(client->scheduler_flush_requested);
    assert(client->current_retry_delay_ms == client->flush_retry_initial_ms);

    storage.connectivity_available = 1;
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.connectivity_calls == 2);
    assert(storage.post_chunk_calls == 1);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_pause_resume_uploads_blocks_flush_without_dropping_events(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 20000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "paused_flush", NULL, 0u) == HONCH_OK);
    assert(storage.queue_depth == 2u);

    assert(honch_core_pause_uploads(client) == HONCH_OK);
    assert(client->uploads_paused);
    assert(honch_core_flush(client) == HONCH_ERROR_OFFLINE);
    assert(storage.post_chunk_calls == 0);
    assert(storage.queue_depth == 2u);

    assert(honch_core_resume_uploads(client) == HONCH_OK);
    assert(!client->uploads_paused);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_tick_while_paused_does_not_keep_flush_pending(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 20000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "paused_tick", NULL, 0u) == HONCH_OK);
    assert(client->scheduler_flush_requested);

    assert(honch_core_pause_uploads(client) == HONCH_OK);
    assert(client->uploads_paused);
    assert(!client->scheduler_flush_requested);

    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 0);
    assert(!client->scheduler_flush_requested);

    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 0);
    assert(!client->scheduler_flush_requested);

    assert(honch_core_resume_uploads(client) == HONCH_OK);
    assert(!client->uploads_paused);
    assert(client->scheduler_flush_requested);
    assert(honch_core_tick(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_flush_returns_offline_before_min_spacing_rate_limit(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 2000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = 10000u;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "first_online_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 1);

    assert(honch_core_track(client, "offline_during_min_spacing", NULL, 0u) == HONCH_OK);
    storage.now_ms = 6000u;
    storage.force_connectivity = 1;
    storage.connectivity_available = 0;
    assert(honch_core_flush(client) == HONCH_ERROR_OFFLINE);
    assert(storage.connectivity_calls == 2);
    assert(storage.post_chunk_calls == 1);
    assert(client->current_retry_delay_ms == client->flush_retry_initial_ms);
    assert(client->next_retry_flush_ms == 0u);
    assert(client->scheduler_flush_requested);

    assert(honch_core_shutdown(client) == HONCH_OK);
}

static void test_zero_min_spacing_allows_back_to_back_flushes(void)
{
    fake_state_storage_t storage = {
        .queue_push_status = HONCH_OK,
        .track_queue_depth = 1,
        .now_ms = 3000u,
        .force_now_ms = 1
    };
    honch_platform_ops_t platform;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport;
    honch_core_config_t config = fake_config(&storage, &platform, &state_ops, &queue_ops, &transport);
    config.flush_event_threshold = 1u;
    config.flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS;

    honch_client_t *client = NULL;
    assert(honch_core_init(&client, &config) == HONCH_OK);
    assert(honch_core_track(client, "first_uncapped_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(honch_core_track(client, "second_uncapped_flush", NULL, 0u) == HONCH_OK);
    assert(honch_core_flush(client) == HONCH_OK);
    assert(storage.post_chunk_calls == 2);
    assert(honch_core_shutdown(client) == HONCH_OK);
}

int main(void)
{
    test_queue_push_uses_honch_event_record_format();
    test_zero_platform_time_queues_parseable_event_record();
    test_pre_wall_clock_track_uses_uptime_timestamp();
    test_core_applies_embedded_defaults_when_tuning_is_omitted();
    test_normal_reset_emits_only_device_boot();
#if HONCH_ENABLE_CRASH_CAPTURE
    test_abnormal_reset_emits_crash_event();
    test_oversized_crash_event_is_skipped_without_failing_init();
    test_overlong_crash_fields_are_omitted_without_large_allocation();
    test_report_crash_emits_crash_event();
    test_report_crash_rejects_null();
    test_report_crash_is_once_only();
    test_crash_uploaded_callback_fires_after_delivery();
    test_crash_uploaded_callback_waits_for_crash_event_delivery();
    test_coredump_streams_and_erases_after_full_delivery();
    test_coredump_completion_latched_when_clear_is_null();
    test_coredump_source_suppresses_summary_erase();
    test_coredump_retry_result_does_not_advance();
    test_coredump_resumes_after_transient_read_error();
    test_nested_flush_during_coredump_post_returns_busy();
    test_coredump_respects_outbound_spacing();
    test_coredump_tick_advances_at_spacing_cadence();
#endif
#if HONCH_ENABLE_LOG_CAPTURE
    test_log_error_emits_error_event();
    test_identical_log_errors_coalesce();
    test_sdk_self_logs_are_ignored();
    test_shutdown_drains_pending_log_errors();
#endif
    test_core_state_lock_works_without_platform_lock_callbacks();
    test_init_rejects_mutex_required_platform_without_lock_callbacks();
    test_failed_firmware_update_queue_does_not_advance_persisted_version();
    test_failed_init_rolls_back_queued_lifecycle_events();
#if HONCH_ENABLE_CRASH_CAPTURE
    test_failed_crash_capture_queue_rolls_back_lifecycle_events();
#endif
    test_failed_reset_second_identity_write_preserves_persisted_identity();
    test_set_property_rejects_blank_key();
    test_set_property_rejects_reserved_key();
    test_track_rejects_promoted_distinct_id_property_key();
    test_init_sets_next_sequence_after_existing_storage_events();
    test_sequence_wrap_rejects_enqueue_without_advancing();
    test_custom_storage_enqueue_updates_cached_queue_depth_without_refresh();
    test_custom_storage_drops_oldest_at_queue_limit();
    test_failed_session_replacement_preserves_old_session();
    test_track_rejects_embedded_nul_property_key();
    test_identify_rejects_embedded_nul_trait_key();
    test_state_get_rejects_size_overflow();
    test_state_get_rejects_inconsistent_read_size();
    test_battery_callback_runs_outside_client_mutex();
    test_auto_properties_callback_runs_outside_client_mutex();
    test_auto_property_buffer_exhaustion_returns_busy();
    test_nested_flush_during_transport_returns_busy();
    test_retry_after_extends_scheduler_delay();
    test_tick_preserves_requested_flush_during_min_spacing();
    test_tick_posts_at_most_one_chunk_per_call();
    test_flush_returns_rate_limited_during_min_spacing();
    test_empty_flush_is_not_rate_limited_during_min_spacing();
    test_tick_skips_transport_while_connectivity_unavailable();
    test_flush_returns_offline_without_transport_attempt();
    test_pause_resume_uploads_blocks_flush_without_dropping_events();
    test_tick_while_paused_does_not_keep_flush_pending();
    test_flush_returns_offline_before_min_spacing_rate_limit();
    test_zero_min_spacing_allows_back_to_back_flushes();
    return 0;
}
