#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char HONCH_BOOT_PROPERTIES[] = "{\"reset_reason\":\"unknown\"}";

static honch_status_t honch_validate_event_name(const char *event_name)
{
    if (honch_is_blank(event_name) || strlen(event_name) > HONCH_MAX_EVENT_NAME) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_validate_distinct_id(const char *distinct_id)
{
    if (honch_is_blank(distinct_id) || strlen(distinct_id) > HONCH_MAX_DISTINCT_ID) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_validate_user_property_key(const char *key)
{
    if (honch_is_blank(key) || honch_property_key_is_reserved(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static bool honch_cstring_exceeds(const char *value, size_t max_length)
{
    if (value == NULL) {
        return false;
    }

    size_t remaining = max_length;
    while (*value != '\0') {
        if (remaining == 0u) {
            return true;
        }
        remaining--;
        value++;
    }
    return false;
}

static honch_status_t honch_validate_json_object_input(
    honch_client_t *client,
    const char *json)
{
    if (honch_cstring_exceeds(json, client->max_event_bytes) || !honch_json_is_object(json)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_validate_json_value_input(
    honch_client_t *client,
    const char *json)
{
    if (honch_cstring_exceeds(json, client->max_event_bytes) || !honch_json_is_value(json)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_append_property_pair(
    honch_buffer_t *buffer,
    size_t *member_count,
    const char *key,
    const char *value)
{
    honch_status_t status = honch_cbor_append_text(buffer, key);
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(buffer, value);
    }
    if (status == HONCH_OK) {
        (*member_count)++;
    }
    return status;
}

static honch_status_t honch_append_raw_property_pair(
    honch_buffer_t *buffer,
    size_t *member_count,
    const char *key,
    const char *value_json)
{
    honch_status_t status = honch_cbor_append_text(buffer, key);
    if (status == HONCH_OK) {
        status = honch_cbor_append_json_value(buffer, value_json);
    }
    if (status == HONCH_OK) {
        (*member_count)++;
    }
    return status;
}

static bool honch_auto_property_key_is_allowed(const char *key)
{
    if (key == NULL) {
        return false;
    }

    return !honch_property_key_is_reserved(key) || strcmp(key, "$wifi_rssi") == 0;
}

typedef struct honch_auto_property_sink_context {
    honch_client_t *client;
    honch_buffer_t *buffer;
    size_t *member_count;
} honch_auto_property_sink_context_t;

typedef struct honch_auto_properties_snapshot {
    honch_buffer_t buffer;
    size_t member_count;
    bool initialized;
} honch_auto_properties_snapshot_t;

typedef struct honch_event_context {
    int battery_level;
    honch_auto_properties_snapshot_t auto_properties;
} honch_event_context_t;

typedef struct honch_lifecycle_queue_tracker {
    uint64_t start_sequence;
    size_t start_queued_event_count;
    uint64_t sequences[8];
    size_t count;
} honch_lifecycle_queue_tracker_t;

static honch_status_t honch_auto_property_sink(
    void *ctx,
    const char *key,
    const char *json_value)
{
    honch_auto_property_sink_context_t *sink_context = (honch_auto_property_sink_context_t *)ctx;
    if (sink_context == NULL || honch_is_blank(key) ||
        honch_validate_json_value_input(sink_context->client, json_value) != HONCH_OK) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (!honch_auto_property_key_is_allowed(key)) {
        return HONCH_OK;
    }

    return honch_append_raw_property_pair(
        sink_context->buffer,
        sink_context->member_count,
        key,
        json_value);
}

static void honch_auto_properties_snapshot_free(honch_auto_properties_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->initialized) {
        return;
    }

    honch_buffer_free(&snapshot->buffer);
    snapshot->member_count = 0u;
    snapshot->initialized = false;
}

static honch_status_t honch_collect_auto_properties(
    honch_client_t *client,
    honch_auto_properties_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *snapshot = (honch_auto_properties_snapshot_t) {0};
    if (client->auto_properties_callback == NULL) {
        return HONCH_OK;
    }

    honch_status_t status = honch_buffer_init(&snapshot->buffer, 128u);
    if (status != HONCH_OK) {
        return status;
    }
    snapshot->initialized = true;

    honch_auto_property_sink_context_t sink_context = {
        .client = client,
        .buffer = &snapshot->buffer,
        .member_count = &snapshot->member_count
    };

    status = client->auto_properties_callback(
        client->auto_properties_userdata,
        honch_auto_property_sink,
        &sink_context);
    if (status != HONCH_OK) {
        honch_auto_properties_snapshot_free(snapshot);
    }
    return status;
}

static honch_status_t honch_append_auto_properties(
    honch_buffer_t *buffer,
    size_t *member_count,
    const honch_auto_properties_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->initialized || snapshot->member_count == 0u) {
        return HONCH_OK;
    }

    honch_status_t status = honch_buffer_append_n(buffer, snapshot->buffer.data, snapshot->buffer.length);
    if (status == HONCH_OK) {
        *member_count += snapshot->member_count;
    }
    return status;
}

static honch_status_t honch_build_property_pairs(
    honch_client_t *client,
    honch_buffer_t *buffer,
    size_t *member_count,
    const char *properties_json,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties)
{
    size_t user_member_count = 0u;
    honch_status_t status = honch_cbor_append_json_object_members(buffer, properties_json, &user_member_count);
    if (status == HONCH_OK) {
        *member_count += user_member_count;
    }

    if (status == HONCH_OK) {
        status = honch_append_auto_properties(buffer, member_count, auto_properties);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, member_count, "$device_id", client->device_id);
    }
    if (status == HONCH_OK && client->session_id != NULL) {
        status = honch_append_property_pair(buffer, member_count, "$session_id", client->session_id);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, member_count, "$device_model", client->device_model);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, member_count, "$firmware_version", client->firmware_version);
    }
    if (status == HONCH_OK && client->environment != NULL) {
        status = honch_append_property_pair(buffer, member_count, "$environment", client->environment);
    }
    if (status == HONCH_OK && battery_level >= 0 && battery_level <= 100) {
        char level_json[4];
        snprintf(level_json, sizeof(level_json), "%d", battery_level);
        status = honch_append_raw_property_pair(buffer, member_count, "$battery_level", level_json);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, member_count, "$sdk_version", HONCH_SDK_VERSION);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, member_count, "$sdk_platform", client->sdk_platform);
    }

    return status;
}

static size_t honch_cbor_map_header_encode(size_t count, char out[9])
{
    uint64_t value = (uint64_t)count;
    if (value < 24u) {
        out[0] = (char)(0xa0u | value);
        return 1u;
    }
    if (value <= UINT8_MAX) {
        out[0] = (char)(0xa0u | 24u);
        out[1] = (char)value;
        return 2u;
    }
    if (value <= UINT16_MAX) {
        out[0] = (char)(0xa0u | 25u);
        out[1] = (char)(value >> 8);
        out[2] = (char)value;
        return 3u;
    }
    if (value <= UINT32_MAX) {
        out[0] = (char)(0xa0u | 26u);
        out[1] = (char)(value >> 24);
        out[2] = (char)(value >> 16);
        out[3] = (char)(value >> 8);
        out[4] = (char)value;
        return 5u;
    }

    out[0] = (char)(0xa0u | 27u);
    out[1] = (char)(value >> 56);
    out[2] = (char)(value >> 48);
    out[3] = (char)(value >> 40);
    out[4] = (char)(value >> 32);
    out[5] = (char)(value >> 24);
    out[6] = (char)(value >> 16);
    out[7] = (char)(value >> 8);
    out[8] = (char)value;
    return 9u;
}

static honch_status_t honch_cbor_patch_map_header(honch_buffer_t *buffer, size_t offset, size_t body_offset, size_t count)
{
    char header[9];
    size_t header_length = honch_cbor_map_header_encode(count, header);
    size_t reserved_length = body_offset - offset;
    if (header_length > reserved_length) {
        size_t extra = header_length - reserved_length;
        size_t needed = 0u;
        honch_status_t status = honch_size_add3(buffer->length, extra, 1u, &needed);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_buffer_reserve(buffer, needed);
        if (status != HONCH_OK) {
            return status;
        }
        size_t body_length = buffer->length - body_offset;
        memmove(buffer->data + body_offset + extra, buffer->data + body_offset, body_length);
        buffer->length += extra;
        buffer->data[buffer->length] = '\0';
    } else if (header_length < reserved_length) {
        size_t body_length = buffer->length - body_offset;
        memmove(buffer->data + offset + header_length, buffer->data + body_offset, body_length);
        buffer->length -= reserved_length - header_length;
        buffer->data[buffer->length] = '\0';
    }
    memcpy(buffer->data + offset, header, header_length);
    return HONCH_OK;
}

static uint64_t honch_client_now_millis(honch_client_t *client);
static honch_status_t honch_client_random_hex(honch_client_t *client, char out[33]);
static honch_status_t honch_client_init_wire_v2_identity(honch_client_t *client);
static honch_status_t honch_client_queue_push_recorded(
    honch_client_t *client,
    const unsigned char *event,
    size_t event_size,
    uint64_t *sequence_out);
static honch_status_t honch_client_queue_consume(honch_client_t *client, uint64_t sequence);
static honch_status_t honch_client_queue_depth(honch_client_t *client, size_t *depth);
static honch_status_t honch_client_queue_clear(honch_client_t *client);
static honch_status_t honch_client_lock(honch_client_t *client);
static void honch_client_unlock(honch_client_t *client);

static honch_status_t honch_build_event(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_payload_t *out)
{
    out->data = NULL;
    out->length = 0u;

    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    if (status == HONCH_OK) {
        status = honch_cbor_append_map(&buffer, 4u);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "event");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, event_name);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "distinct_id");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, client->distinct_id);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "timestamp");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_int(&buffer, (int64_t)honch_client_now_millis(client));
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "properties");
    }

    size_t property_map_offset = buffer.length;
    char property_map_placeholder = 0;
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(&buffer, &property_map_placeholder, 1u);
    }
    size_t property_body_offset = buffer.length;
    size_t property_count = 0u;
    if (status == HONCH_OK) {
        status = honch_build_property_pairs(
            client,
            &buffer,
            &property_count,
            properties_json,
            battery_level,
            auto_properties);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_patch_map_header(&buffer, property_map_offset, property_body_offset, property_count);
    }

    if (status == HONCH_OK && buffer.length > client->max_event_bytes) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    out->data = (unsigned char *)buffer.data;
    out->length = buffer.length;
    return HONCH_OK;
}

static int honch_read_battery_level(honch_client_t *client)
{
    if (client->battery_callback == NULL) {
        return -1;
    }

    return client->battery_callback();
}

static void honch_event_context_free(honch_event_context_t *event_context)
{
    if (event_context == NULL) {
        return;
    }

    honch_auto_properties_snapshot_free(&event_context->auto_properties);
    event_context->battery_level = -1;
}

static honch_status_t honch_prepare_event_context(honch_client_t *client, honch_event_context_t *event_context)
{
    if (client == NULL || event_context == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *event_context = (honch_event_context_t) {
        .battery_level = honch_read_battery_level(client)
    };

    honch_status_t status = honch_collect_auto_properties(client, &event_context->auto_properties);
    if (status != HONCH_OK) {
        honch_event_context_free(event_context);
    }
    return status;
}

static uint64_t honch_client_now_millis(honch_client_t *client)
{
    if (client != NULL && client->platform != NULL && client->platform->now_ms != NULL) {
        return client->platform->now_ms(client->platform->ctx);
    }

    return honch_now_millis();
}

static honch_status_t honch_client_random_hex(honch_client_t *client, char out[33])
{
    if (client == NULL || out == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (client->platform == NULL || client->platform->random_bytes == NULL) {
        return honch_random_hex(out);
    }

    unsigned char bytes[16];
    honch_status_t status = client->platform->random_bytes(client->platform->ctx, bytes, sizeof(bytes));
    if (status != HONCH_OK) {
        return status;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(bytes); i++) {
        out[i * 2u] = hex[(bytes[i] >> 4u) & 0x0fu];
        out[(i * 2u) + 1u] = hex[bytes[i] & 0x0fu];
    }
    out[32] = '\0';
    return HONCH_OK;
}

static uint32_t honch_hex_u32_prefix(const char hex[33])
{
    uint32_t value = 0u;
    for (size_t i = 0u; i < 8u; i++) {
        char c = hex[i];
        uint32_t nibble = 0u;
        if (c >= '0' && c <= '9') {
            nibble = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint32_t)(c - 'A' + 10);
        }
        value = (value << 4u) | nibble;
    }
    return value;
}

static honch_status_t honch_client_init_wire_v2_identity(honch_client_t *client)
{
    char random[33];
    honch_status_t status = honch_client_random_hex(client, random);
    if (status != HONCH_OK) {
        return status;
    }

    memcpy(client->wire_v2_stream_id, random, 8u);
    client->wire_v2_stream_id[8] = '\0';
    client->wire_v2_message_id_seed = honch_hex_u32_prefix(random);
    return HONCH_OK;
}

static honch_status_t honch_client_queue_push_recorded(
    honch_client_t *client,
    const unsigned char *event,
    size_t event_size,
    uint64_t *sequence_out)
{
    if (client == NULL || client->sequence == UINT64_MAX) {
        return HONCH_ERROR_QUEUE_FULL;
    }

    if (client != NULL && client->storage != NULL && client->storage->queue_push != NULL) {
        uint64_t sequence = client->sequence;
        honch_status_t status = client->storage->queue_push(client->storage->ctx, event, event_size, sequence);
        if (status == HONCH_OK) {
            client->sequence++;
            if (sequence_out != NULL) {
                *sequence_out = sequence;
            }
            size_t depth = 0u;
            if (honch_client_queue_depth(client, &depth) == HONCH_OK) {
                client->queued_event_count = depth;
            } else if (client->queued_event_count < SIZE_MAX) {
                client->queued_event_count++;
            }
        }
        return status;
    }

    uint64_t sequence = client->sequence;
    honch_status_t status = honch_queue_enqueue(client, event, event_size);
    if (status == HONCH_OK && sequence_out != NULL) {
        *sequence_out = sequence;
    }
    return status;
}

static honch_status_t honch_client_queue_consume(honch_client_t *client, uint64_t sequence)
{
    if (client != NULL && client->storage != NULL && client->storage->queue_consume != NULL) {
        return client->storage->queue_consume(client->storage->ctx, sequence);
    }

    return HONCH_ERROR_INVALID_ARGUMENT;
}

static void honch_lifecycle_queue_tracker_begin(
    honch_client_t *client,
    honch_lifecycle_queue_tracker_t *tracker)
{
    if (tracker == NULL) {
        return;
    }

    *tracker = (honch_lifecycle_queue_tracker_t) {
        .start_sequence = client != NULL ? client->sequence : 0u,
        .start_queued_event_count = client != NULL ? client->queued_event_count : 0u
    };
}

static honch_status_t honch_lifecycle_queue_tracker_record(
    honch_lifecycle_queue_tracker_t *tracker,
    uint64_t sequence)
{
    if (tracker == NULL) {
        return HONCH_OK;
    }
    if (tracker->count >= sizeof(tracker->sequences) / sizeof(tracker->sequences[0])) {
        return HONCH_ERROR_QUEUE_FULL;
    }

    tracker->sequences[tracker->count++] = sequence;
    return HONCH_OK;
}

static honch_status_t honch_lifecycle_queue_tracker_rollback(
    honch_client_t *client,
    honch_lifecycle_queue_tracker_t *tracker)
{
    if (client == NULL || tracker == NULL) {
        return HONCH_OK;
    }

    honch_status_t status = HONCH_OK;
    for (size_t i = tracker->count; i > 0u; i--) {
        honch_status_t consume_status = honch_client_queue_consume(client, tracker->sequences[i - 1u]);
        if (status == HONCH_OK && consume_status != HONCH_OK) {
            status = consume_status;
        }
    }

    client->sequence = tracker->start_sequence;
    size_t depth = 0u;
    if (honch_client_queue_depth(client, &depth) == HONCH_OK) {
        client->queued_event_count = depth;
    } else {
        client->queued_event_count = tracker->start_queued_event_count;
    }
    tracker->count = 0u;
    return status;
}

static honch_status_t honch_client_queue_depth(honch_client_t *client, size_t *depth)
{
    if (client != NULL && client->storage != NULL && client->storage->queue_depth != NULL) {
        return client->storage->queue_depth(client->storage->ctx, depth);
    }

    return honch_queue_count_pending(client, depth);
}

static honch_status_t honch_client_queue_clear(honch_client_t *client)
{
    if (client != NULL && client->storage != NULL && client->storage->queue_clear != NULL) {
        return client->storage->queue_clear(client->storage->ctx);
    }

    return honch_queue_clear(client);
}

static honch_status_t honch_client_lock(honch_client_t *client)
{
    return honch_client_state_lock(client);
}

static void honch_client_unlock(honch_client_t *client)
{
    honch_client_state_unlock(client);
}

static honch_status_t honch_track_locked_internal(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    int battery_level,
    bool check_battery_low,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker);
static void honch_scheduler_notify_after_enqueue_locked(honch_client_t *client);

static honch_status_t honch_emit_battery_low_locked(
    honch_client_t *client,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    if (battery_level < 0) {
        return HONCH_OK;
    }

    if (battery_level >= client->battery_low_threshold) {
        client->battery_low_emitted = false;
        return HONCH_OK;
    }

    if (client->battery_low_emitted) {
        return HONCH_OK;
    }

    client->battery_low_emitted = true;
    char properties_json[32];
    snprintf(properties_json, sizeof(properties_json), "{\"level\":%d}", battery_level);
    return honch_track_locked_internal(
        client,
        "$battery_low",
        properties_json,
        battery_level,
        false,
        auto_properties,
        lifecycle_tracker);
}

static honch_status_t honch_track_locked_internal(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    int battery_level,
    bool check_battery_low,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    honch_payload_t event = {0};
    honch_status_t status = honch_build_event(
        client,
        event_name,
        properties_json,
        battery_level,
        auto_properties,
        &event);
    uint64_t sequence = 0u;
    if (status == HONCH_OK) {
        status = honch_client_queue_push_recorded(client, event.data, event.length, &sequence);
    }
    if (status == HONCH_OK) {
        status = honch_lifecycle_queue_tracker_record(lifecycle_tracker, sequence);
    }
    if (status == HONCH_OK) {
        honch_scheduler_notify_after_enqueue_locked(client);
    }
    if (status == HONCH_OK && check_battery_low) {
        status = honch_emit_battery_low_locked(client, battery_level, auto_properties, lifecycle_tracker);
    }

    free(event.data);
    return status;
}

static uint64_t honch_scheduler_interval_ms(honch_client_t *client)
{
    return (uint64_t)client->flush_interval_seconds * 1000u;
}

static unsigned int honch_next_retry_delay_ms(honch_client_t *client)
{
    unsigned int delay = client->current_retry_delay_ms;
    if (delay == 0u) {
        delay = client->flush_retry_initial_ms;
    }

    unsigned int quarter = delay / 4u;
    if (quarter == 0u) {
        return delay;
    }

    uint64_t now = honch_client_now_millis(client);
    unsigned int jitter = (unsigned int)(now % ((uint64_t)(quarter * 2u) + 1u));
    return (delay - quarter) + jitter;
}

static void honch_grow_retry_delay(honch_client_t *client)
{
    unsigned int next = client->current_retry_delay_ms == 0u ?
        client->flush_retry_initial_ms :
        client->current_retry_delay_ms * 2u;
    if (next < client->current_retry_delay_ms || next > client->flush_retry_max_ms) {
        next = client->flush_retry_max_ms;
    }
    client->current_retry_delay_ms = next;
}

static bool honch_status_is_retryable(honch_status_t status)
{
    return status == HONCH_ERROR_TRANSPORT ||
           status == HONCH_ERROR_RATE_LIMITED ||
           status == HONCH_ERROR_TIMEOUT ||
           status == HONCH_ERROR_SERVER;
}

static void honch_scheduler_make_deadline(struct timespec *deadline, uint64_t wait_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(wait_ms / 1000u);
    deadline->tv_nsec += (long)((wait_ms % 1000u) * 1000000u);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static uint64_t honch_scheduler_wait_ms(honch_client_t *client, uint64_t now)
{
    if (client->next_retry_flush_ms > now) {
        return client->next_retry_flush_ms - now;
    }

    uint64_t wait_ms = UINT64_MAX;
    if (client->scheduler_flush_requested) {
        wait_ms = 0u;
    }
    if (client->flush_interval_seconds > 0u) {
        uint64_t interval_wait = client->next_interval_flush_ms > now ?
            client->next_interval_flush_ms - now :
            0u;
        if (interval_wait < wait_ms) {
            wait_ms = interval_wait;
        }
    }
    return wait_ms;
}

static void honch_scheduler_notify_after_enqueue_locked(honch_client_t *client)
{
    if (!client->scheduler_enabled || client->flush_event_threshold == 0u) {
        return;
    }

    if (client->queued_event_count >= client->flush_event_threshold) {
        client->scheduler_flush_requested = true;
        if (client->scheduler_started) {
            pthread_cond_signal(&client->scheduler_cond);
        }
    }
}

static void *honch_scheduler_main(void *userdata)
{
    honch_client_t *client = (honch_client_t *)userdata;
    pthread_mutex_lock(&client->mutex);

    while (!client->scheduler_stop) {
        uint64_t now = honch_client_now_millis(client);
        bool retry_blocked = client->next_retry_flush_ms > now;
        bool interval_due = client->flush_interval_seconds > 0u && now >= client->next_interval_flush_ms;
        bool should_flush = (client->scheduler_flush_requested || interval_due) && !retry_blocked;

        if (should_flush) {
            client->scheduler_flush_requested = false;
            honch_status_t status = honch_queue_flush_locked(client);
            now = honch_client_now_millis(client);

            if (status == HONCH_OK) {
                client->current_retry_delay_ms = client->flush_retry_initial_ms;
                client->next_retry_flush_ms = 0u;
            } else if (honch_status_is_retryable(status)) {
                uint64_t wait_ms = honch_next_retry_delay_ms(client);
                client->next_retry_flush_ms = now + wait_ms;
                client->scheduler_flush_requested = true;
                honch_grow_retry_delay(client);
            }

            if (client->flush_interval_seconds > 0u) {
                client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
            }
            continue;
        }

        uint64_t wait_ms = honch_scheduler_wait_ms(client, now);
        if (wait_ms == UINT64_MAX) {
            pthread_cond_wait(&client->scheduler_cond, &client->mutex);
        } else {
            struct timespec deadline;
            honch_scheduler_make_deadline(&deadline, wait_ms);
            pthread_cond_timedwait(&client->scheduler_cond, &client->mutex, &deadline);
        }
    }

    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

static honch_status_t honch_scheduler_start(honch_client_t *client)
{
    if (!client->scheduler_enabled) {
        return HONCH_OK;
    }

    uint64_t now = honch_client_now_millis(client);
    client->current_retry_delay_ms = client->flush_retry_initial_ms;
    if (client->flush_interval_seconds > 0u) {
        client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
    }

    if (client->flush_event_threshold > 0u) {
        size_t pending_count = 0u;
        honch_status_t status = honch_client_queue_depth(client, &pending_count);
        if (status != HONCH_OK) {
            return status;
        }
        client->scheduler_flush_requested = pending_count >= client->flush_event_threshold;
    }

    if (pthread_create(&client->scheduler_thread, NULL, honch_scheduler_main, client) != 0) {
        return HONCH_ERROR_IO;
    }
    client->scheduler_started = true;
    return HONCH_OK;
}

static void honch_scheduler_stop(honch_client_t *client)
{
    if (!client->scheduler_started) {
        return;
    }

    pthread_mutex_lock(&client->mutex);
    client->scheduler_stop = true;
    pthread_cond_signal(&client->scheduler_cond);
    pthread_mutex_unlock(&client->mutex);

    pthread_join(client->scheduler_thread, NULL);
    client->scheduler_started = false;
}

static honch_status_t honch_new_session_id(honch_client_t *client, char **out)
{
    char random[33];
    honch_status_t status = honch_client_random_hex(client, random);
    if (status != HONCH_OK) {
        return status;
    }

    char *session_id = (char *)malloc(38u);
    if (session_id == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    snprintf(session_id, 38u, "sess_%s", random);
    *out = session_id;
    return HONCH_OK;
}

static honch_status_t honch_build_session_start_properties(const char *session_name, char **out)
{
    *out = NULL;
    if (honch_is_blank(session_name)) {
        return HONCH_OK;
    }

    honch_buffer_t properties;
    size_t initial_capacity = 0u;
    honch_status_t status = honch_size_add(strlen(session_name), 32u, &initial_capacity);
    if (status == HONCH_OK) {
        status = honch_buffer_init(&properties, initial_capacity);
    }
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&properties, "{\"session_name\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, session_name);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&properties);
        return status;
    }

    *out = properties.data;
    return HONCH_OK;
}

static honch_status_t honch_build_firmware_update_properties(
    const char *previous_version,
    const char *new_version,
    char **out)
{
    size_t initial_capacity = 0u;
    honch_status_t status = honch_size_add3(
        strlen(previous_version),
        strlen(new_version),
        64u,
        &initial_capacity);
    if (status != HONCH_OK) {
        return status;
    }

    honch_buffer_t properties;
    status = honch_buffer_init(&properties, initial_capacity);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&properties, "{\"previous_version\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, previous_version);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, ",\"new_version\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, new_version);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&properties);
        return status;
    }

    *out = properties.data;
    return HONCH_OK;
}

static honch_status_t honch_emit_firmware_update_locked(
    honch_client_t *client,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker,
    bool *firmware_version_pending_save)
{
    if (firmware_version_pending_save != NULL) {
        *firmware_version_pending_save = false;
    }

    bool changed = false;
    char *previous_version = NULL;
    honch_status_t status = honch_state_check_firmware_version(client, &changed, &previous_version);
    if (status != HONCH_OK) {
        free(previous_version);
        return status;
    }
    if (!changed) {
        free(previous_version);
        return honch_state_save_firmware_version(client);
    }

    char *properties_json = NULL;
    status = honch_build_firmware_update_properties(previous_version, client->firmware_version, &properties_json);
    if (status == HONCH_OK) {
        honch_event_context_t event_context = {.battery_level = -1};
        status = honch_prepare_event_context(client, &event_context);
        if (status == HONCH_OK) {
            status = honch_track_locked_internal(
                client,
                "$firmware_update",
                properties_json,
                event_context.battery_level,
                true,
                &event_context.auto_properties,
                lifecycle_tracker);
        }
        honch_event_context_free(&event_context);
    }
    if (status == HONCH_OK) {
        if (firmware_version_pending_save != NULL) {
            *firmware_version_pending_save = true;
        } else {
            status = honch_state_save_firmware_version(client);
        }
    }

    free(properties_json);
    free(previous_version);
    return status;
}

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config)
{
    if (client == NULL || config == NULL || honch_is_blank(config->api_key) ||
        honch_is_blank(config->endpoint_url) || honch_is_blank(config->device_model) ||
        honch_is_blank(config->firmware_version) || honch_is_blank(config->queue_directory) ||
        (config->durability_mode != HONCH_DURABILITY_SYNC_ALWAYS &&
            config->durability_mode != HONCH_DURABILITY_OS_BUFFERED)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *client = NULL;

    honch_client_t *next = (honch_client_t *)calloc(1u, sizeof(*next));
    if (next == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (config->platform != NULL) {
        next->platform_ops = *config->platform;
        if (next->platform_ops.ctx == NULL) {
            next->platform_ops.ctx = next;
        }
        next->platform = &next->platform_ops;
    }
    if (config->storage != NULL) {
        next->storage_ops = *config->storage;
        if (next->storage_ops.ctx == NULL) {
            next->storage_ops.ctx = next;
        }
        next->storage = &next->storage_ops;
    }
    if (config->transport != NULL) {
        next->transport_ops = *config->transport;
        if (next->transport_ops.ctx == NULL) {
            next->transport_ops.ctx = next;
        }
        next->transport = &next->transport_ops;
    }

    next->api_key = honch_strdup(config->api_key);
    next->endpoint_url = honch_strdup(config->endpoint_url);
    next->queue_directory = honch_strdup(config->queue_directory);
    next->sdk_platform = honch_strdup(honch_is_blank(config->sdk_platform) ? "c-posix" : config->sdk_platform);
    next->batch_size = config->batch_size == 0u ? HONCH_DEFAULT_BATCH_SIZE : config->batch_size;
    if (next->batch_size > HONCH_MAX_BATCH_SIZE) {
        next->batch_size = HONCH_MAX_BATCH_SIZE;
    }
    next->max_queued_events = config->max_queued_events == 0u ? HONCH_DEFAULT_MAX_QUEUED_EVENTS : config->max_queued_events;
    next->max_event_bytes = config->max_event_bytes == 0u ? HONCH_DEFAULT_MAX_EVENT_BYTES : config->max_event_bytes;
    next->transport_timeout_ms = config->transport_timeout_ms == 0u ?
        HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS :
        config->transport_timeout_ms;
    next->flush_interval_seconds = config->flush_interval_seconds == 0u ?
        HONCH_DEFAULT_FLUSH_INTERVAL_SECONDS :
        config->flush_interval_seconds;
    next->flush_event_threshold = config->flush_event_threshold == 0u ?
        HONCH_DEFAULT_FLUSH_EVENT_THRESHOLD :
        config->flush_event_threshold;
    next->flush_retry_initial_ms = config->flush_retry_initial_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS :
        config->flush_retry_initial_ms;
    next->flush_retry_max_ms = config->flush_retry_max_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_MAX_MS :
        config->flush_retry_max_ms;
    if (next->flush_retry_max_ms < next->flush_retry_initial_ms) {
        next->flush_retry_max_ms = next->flush_retry_initial_ms;
    }
    next->durability_mode = config->durability_mode;
    next->scheduler_enabled = config->disable_background_flush == 0 &&
        (next->flush_interval_seconds > 0u || next->flush_event_threshold > 0u);
    next->battery_callback = config->battery_callback;
    next->battery_low_threshold = config->battery_low_threshold > 0 ?
        config->battery_low_threshold :
        HONCH_DEFAULT_BATTERY_LOW_THRESHOLD;
    next->auto_properties_callback = config->auto_properties_callback;
    next->auto_properties_userdata = config->auto_properties_userdata;

    honch_status_t status = HONCH_OK;
    bool lifetime_mutex_initialized = false;
    bool lifetime_cond_initialized = false;
    bool mutex_initialized = false;
    bool scheduler_cond_initialized = false;
    bool firmware_version_pending_save = false;
    honch_lifecycle_queue_tracker_t lifecycle_tracker = {0};
    if (next->api_key == NULL || next->endpoint_url == NULL || next->queue_directory == NULL ||
        next->sdk_platform == NULL) {
        status = HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (status == HONCH_OK) {
        status = honch_state_prepare(next, config);
    }
    if (status == HONCH_OK) {
        status = honch_client_init_wire_v2_identity(next);
    }
    if (status == HONCH_OK) {
        status = honch_client_queue_depth(next, &next->queued_event_count);
    }
    if (status == HONCH_OK && pthread_mutex_init(&next->lifetime_mutex, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        lifetime_mutex_initialized = true;
    }
    if (status == HONCH_OK && pthread_cond_init(&next->lifetime_cond, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        lifetime_cond_initialized = true;
    }
    if (status == HONCH_OK && pthread_mutex_init(&next->mutex, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        mutex_initialized = true;
    }
    if (status == HONCH_OK && pthread_cond_init(&next->scheduler_cond, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        scheduler_cond_initialized = true;
    }
    if (status == HONCH_OK) {
        honch_lifecycle_queue_tracker_begin(next, &lifecycle_tracker);
        status = honch_emit_firmware_update_locked(next, &lifecycle_tracker, &firmware_version_pending_save);
    }
    if (status == HONCH_OK) {
        honch_event_context_t event_context = {.battery_level = -1};
        status = honch_prepare_event_context(next, &event_context);
        if (status == HONCH_OK) {
            status = honch_track_locked_internal(
                next,
                "$device_boot",
                HONCH_BOOT_PROPERTIES,
                event_context.battery_level,
                true,
                &event_context.auto_properties,
                &lifecycle_tracker);
        }
        honch_event_context_free(&event_context);
    }
    if (status == HONCH_OK && firmware_version_pending_save) {
        status = honch_state_save_firmware_version(next);
    }
    if (status == HONCH_OK) {
        status = honch_scheduler_start(next);
    }

    if (status != HONCH_OK) {
        if (lifecycle_tracker.count > 0u) {
            honch_status_t rollback_status = honch_lifecycle_queue_tracker_rollback(next, &lifecycle_tracker);
            if (rollback_status != HONCH_OK) {
                status = rollback_status;
            }
        }
        honch_scheduler_stop(next);
        if (scheduler_cond_initialized) {
            pthread_cond_destroy(&next->scheduler_cond);
        }
        if (mutex_initialized) {
            pthread_mutex_destroy(&next->mutex);
        }
        if (lifetime_cond_initialized) {
            pthread_cond_destroy(&next->lifetime_cond);
        }
        if (lifetime_mutex_initialized) {
            pthread_mutex_destroy(&next->lifetime_mutex);
        }
        honch_free_client_fields(next);
        free(next);
        return status;
    }

    *client = next;
    return HONCH_OK;
}

honch_status_t honch_core_track(honch_client_t *client, const char *event_name, const char *properties_json)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_event_name(event_name) != HONCH_OK ||
        honch_cstring_exceeds(properties_json, client->max_event_bytes)) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return status;
    }

    status = honch_track_locked_internal(
        client,
        event_name,
        properties_json,
        event_context.battery_level,
        true,
        &event_context.auto_properties,
        NULL);
    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_identify(honch_client_t *client, const char *distinct_id, const char *traits_json)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_distinct_id(distinct_id) != HONCH_OK ||
        honch_validate_json_object_input(client, traits_json) != HONCH_OK) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return status;
    }

    char *previous_distinct_id = honch_strdup(client->distinct_id);
    char *next_distinct_id = honch_strdup(distinct_id);
    if (previous_distinct_id == NULL || next_distinct_id == NULL) {
        free(previous_distinct_id);
        free(next_distinct_id);
        honch_client_unlock(client);
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    status = honch_state_save_distinct_id_value(client, next_distinct_id);
    if (status != HONCH_OK) {
        free(previous_distinct_id);
        free(next_distinct_id);
        honch_client_unlock(client);
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return status;
    }

    free(client->distinct_id);
    client->distinct_id = next_distinct_id;
    next_distinct_id = NULL;

    status = honch_track_locked_internal(
        client,
        "$identify",
        traits_json,
        event_context.battery_level,
        false,
        &event_context.auto_properties,
        NULL);
    if (status != HONCH_OK) {
        honch_status_t rollback_status = honch_state_save_distinct_id_value(client, previous_distinct_id);
        if (rollback_status == HONCH_OK) {
            free(client->distinct_id);
            client->distinct_id = previous_distinct_id;
            previous_distinct_id = NULL;
        } else {
            status = rollback_status;
        }
    }

    free(previous_distinct_id);
    free(next_distinct_id);
    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_set_property(honch_client_t *client, const char *key, const char *value_json)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_user_property_key(key) != HONCH_OK ||
        honch_validate_json_value_input(client, value_json) != HONCH_OK) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    const char *value = value_json == NULL ? "null" : value_json;
    honch_buffer_t properties;
    size_t initial_capacity = 0u;
    status = honch_size_add3(strlen(key), strlen(value), 16u, &initial_capacity);
    if (status == HONCH_OK) {
        status = honch_buffer_init(&properties, initial_capacity);
    }
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_buffer_append(&properties, "{");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, key);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, ":");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, value);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }
    honch_event_context_t event_context = {.battery_level = -1};
    if (status == HONCH_OK) {
        status = honch_prepare_event_context(client, &event_context);
    }
    if (status == HONCH_OK) {
        status = honch_client_lock(client);
        if (status == HONCH_OK) {
            status = honch_track_locked_internal(
                client,
                "$set_property",
                properties.data,
                event_context.battery_level,
                true,
                &event_context.auto_properties,
                NULL);
            honch_client_unlock(client);
        }
    }

    honch_event_context_free(&event_context);
    honch_buffer_free(&properties);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    char *session_id = NULL;
    status = honch_new_session_id(client, &session_id);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    char *properties_json = NULL;
    status = honch_build_session_start_properties(session_name, &properties_json);
    if (status != HONCH_OK) {
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    honch_event_context_t end_event_context = {.battery_level = -1};
    honch_event_context_t start_event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &end_event_context);
    if (status == HONCH_OK) {
        status = honch_prepare_event_context(client, &start_event_context);
    }
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(properties_json);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(properties_json);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    if (client->session_id != NULL) {
        status = honch_track_locked_internal(
            client,
            "$session_end",
            NULL,
            end_event_context.battery_level,
            true,
            &end_event_context.auto_properties,
            NULL);
        if (status == HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }
    if (status == HONCH_OK) {
        client->session_id = session_id;
        session_id = NULL;
        status = honch_track_locked_internal(
            client,
            "$session_start",
            properties_json,
            start_event_context.battery_level,
            true,
            &start_event_context.auto_properties,
            NULL);
        if (status != HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }

    honch_client_unlock(client);
    honch_event_context_free(&end_event_context);
    honch_event_context_free(&start_event_context);
    free(properties_json);
    free(session_id);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_session_end(honch_client_t *client)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return status;
    }

    if (client->session_id != NULL) {
        status = honch_track_locked_internal(
            client,
            "$session_end",
            NULL,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            NULL);
        if (status == HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }

    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_flush(honch_client_t *client)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_queue_flush_locked(client);
    honch_client_unlock(client);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_reset(honch_client_t *client)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_state_reset(client);
    if (status == HONCH_OK) {
        free(client->session_id);
        client->session_id = NULL;
        client->battery_low_emitted = false;
    }
    if (status == HONCH_OK) {
        status = honch_client_queue_clear(client);
    }
    honch_client_unlock(client);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_shutdown(honch_client_t *client)
{
    honch_status_t status = honch_client_begin_shutdown(client);

    if (status != HONCH_OK) {
        return status;
    }

    honch_scheduler_stop(client);

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        pthread_cond_destroy(&client->scheduler_cond);
        pthread_mutex_destroy(&client->mutex);
        pthread_cond_destroy(&client->lifetime_cond);
        pthread_mutex_destroy(&client->lifetime_mutex);
        honch_free_client_fields(client);
        free(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        pthread_cond_destroy(&client->scheduler_cond);
        pthread_mutex_destroy(&client->mutex);
        pthread_cond_destroy(&client->lifetime_cond);
        pthread_mutex_destroy(&client->lifetime_mutex);
        honch_free_client_fields(client);
        free(client);
        return status;
    }

    status = honch_track_locked_internal(
        client,
        "$device_shutdown",
        NULL,
        event_context.battery_level,
        true,
        &event_context.auto_properties,
        NULL);
    honch_status_t flush_status = honch_queue_flush_locked(client);
    if (status == HONCH_OK) {
        status = flush_status;
    }
    honch_client_unlock(client);
    honch_event_context_free(&event_context);

    pthread_cond_destroy(&client->scheduler_cond);
    pthread_mutex_destroy(&client->mutex);
    pthread_cond_destroy(&client->lifetime_cond);
    pthread_mutex_destroy(&client->lifetime_mutex);
    honch_free_client_fields(client);
    free(client);
    return status;
}

const char *honch_core_get_device_id(honch_client_t *client)
{
    if (honch_client_enter(client) != HONCH_OK) {
        return NULL;
    }

    const char *device_id = client->device_id;
    honch_client_leave(client);
    return device_id;
}

honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (buffer == NULL || buffer_size == 0u) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    size_t length = strlen(client->device_id);
    size_t needed = 0u;
    status = honch_size_add(length, 1u, &needed);
    if (status == HONCH_OK && needed > buffer_size) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (status == HONCH_OK) {
        memcpy(buffer, client->device_id, needed);
    } else {
        buffer[0] = '\0';
    }

    honch_client_unlock(client);
    honch_client_leave(client);
    return status;
}

const char *honch_status_string(honch_status_t status)
{
    switch (status) {
        case HONCH_OK:
            return "ok";
        case HONCH_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case HONCH_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case HONCH_ERROR_IO:
            return "io error";
        case HONCH_ERROR_TRANSPORT:
            return "transport error";
        case HONCH_ERROR_RATE_LIMITED:
            return "rate limited";
        case HONCH_ERROR_SERVER:
            return "server error";
        case HONCH_ERROR_REJECTED:
            return "rejected";
        case HONCH_ERROR_NOT_INITIALIZED:
            return "not initialized";
        case HONCH_ERROR_ALREADY_INITIALIZED:
            return "already initialized";
        case HONCH_ERROR_QUEUE_FULL:
            return "queue full";
        case HONCH_ERROR_TIMEOUT:
            return "timeout";
        case HONCH_ERROR_INTERNAL:
            return "internal error";
        default:
            return "unknown";
    }
}
