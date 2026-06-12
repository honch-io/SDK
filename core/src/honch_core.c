#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static bool honch_auto_property_key_is_allowed(const char *key)
{
    if (key == NULL) {
        return false;
    }

    return !honch_property_key_is_reserved(key) || strcmp(key, "$wifi_rssi") == 0;
}

typedef struct honch_auto_property_sink_context {
    honch_wire_v2_property_t *properties;
    size_t *property_count;
} honch_auto_property_sink_context_t;

typedef struct honch_auto_properties_snapshot {
    honch_wire_v2_property_t *properties;
    size_t property_count;
    honch_client_t *client;
    size_t buffer_index;
    bool buffer_acquired;
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

static honch_status_t honch_append_typed_property(
    honch_wire_v2_property_t *properties,
    size_t *property_count,
    const char *key,
    honch_wire_v2_value_t value,
    bool allow_reserved)
{
    if (properties == NULL || property_count == NULL || honch_is_blank(key) ||
        (!allow_reserved && honch_property_key_is_reserved(key)) ||
        *property_count >= HONCH_MAX_EVENT_PROPERTIES) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < *property_count; i++) {
        if (strcmp(properties[i].key, key) == 0) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
    }
    properties[*property_count] = honch_prop(key, value);
    (*property_count)++;
    return HONCH_OK;
}

static honch_status_t honch_auto_property_sink(
    void *ctx,
    const char *key,
    honch_wire_v2_value_t value)
{
    honch_auto_property_sink_context_t *sink_context = (honch_auto_property_sink_context_t *)ctx;
    if (sink_context == NULL || honch_is_blank(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (!honch_auto_property_key_is_allowed(key)) {
        return HONCH_OK;
    }

    return honch_append_typed_property(
        sink_context->properties,
        sink_context->property_count,
        key,
        value,
        true);
}

static honch_status_t honch_acquire_auto_property_buffer(
    honch_client_t *client,
    honch_auto_properties_snapshot_t *snapshot)
{
    if (client == NULL || snapshot == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    for (size_t i = 0u; i < HONCH_AUTO_PROPERTY_BUFFER_COUNT; i++) {
        bool expected = false;
        if (honch_atomic_bool_compare_exchange(
                &client->auto_property_buffer_in_use[i],
                &expected,
                true)) {
            snapshot->properties = client->auto_property_buffers[i];
            snapshot->client = client;
            snapshot->buffer_index = i;
            snapshot->buffer_acquired = true;
            memset(snapshot->properties, 0, sizeof(client->auto_property_buffers[i]));
            return HONCH_OK;
        }
    }

    return HONCH_ERROR_BUSY;
}

static void honch_auto_properties_snapshot_free(honch_auto_properties_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    if (snapshot->buffer_acquired && snapshot->client != NULL &&
        snapshot->buffer_index < HONCH_AUTO_PROPERTY_BUFFER_COUNT) {
        honch_atomic_bool_store(
            &snapshot->client->auto_property_buffer_in_use[snapshot->buffer_index],
            false);
    }
    memset(snapshot, 0, sizeof(*snapshot));
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

    honch_status_t status = honch_acquire_auto_property_buffer(client, snapshot);
    if (status != HONCH_OK) {
        return status;
    }

    honch_auto_property_sink_context_t sink_context = {
        .properties = snapshot->properties,
        .property_count = &snapshot->property_count
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
    honch_wire_v2_property_t *properties,
    size_t *property_count,
    const honch_auto_properties_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->property_count == 0u) {
        return HONCH_OK;
    }
    for (size_t i = 0u; i < snapshot->property_count; i++) {
        honch_status_t status = honch_append_typed_property(
            properties,
            property_count,
            snapshot->properties[i].key,
            snapshot->properties[i].value,
            true);
        if (status != HONCH_OK) {
            return status;
        }
    }
    return HONCH_OK;
}

static honch_status_t honch_build_property_pairs(
    honch_wire_v2_property_t *out_properties,
    size_t *out_property_count,
    const honch_wire_v2_property_t *user_properties,
    size_t user_property_count,
    const honch_wire_v2_property_t *trusted_properties,
    size_t trusted_property_count,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties)
{
    *out_property_count = 0u;
    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < user_property_count; i++) {
        status = honch_append_typed_property(
            out_properties,
            out_property_count,
            user_properties[i].key,
            user_properties[i].value,
            false);
    }
    for (size_t i = 0u; status == HONCH_OK && i < trusted_property_count; i++) {
        status = honch_append_typed_property(
            out_properties,
            out_property_count,
            trusted_properties[i].key,
            trusted_properties[i].value,
            true);
    }
    if (status == HONCH_OK) {
        status = honch_append_auto_properties(out_properties, out_property_count, auto_properties);
    }
    if (status == HONCH_OK && battery_level >= 0 && battery_level <= 100) {
        status = honch_append_typed_property(out_properties, out_property_count, "$battery_level", honch_i64(battery_level), true);
    }

    return status;
}

static uint64_t honch_client_now_millis(honch_client_t *client);
static uint64_t honch_client_event_timestamp_millis(honch_client_t *client);
static honch_status_t honch_client_random_hex(honch_client_t *client, char out[33]);
static honch_status_t honch_client_init_wire_v2_identity(honch_client_t *client);
static honch_status_t honch_client_queue_push_recorded(
    honch_client_t *client,
    const unsigned char *event,
    size_t event_size,
    uint64_t *sequence_out);
static honch_status_t honch_client_queue_consume(honch_client_t *client, uint64_t sequence);
static honch_status_t honch_client_queue_depth(honch_client_t *client, size_t *depth);
static honch_status_t honch_core_sync_sequence_from_storage(honch_client_t *client, size_t depth);
static honch_status_t honch_client_queue_clear(honch_client_t *client);
static honch_status_t honch_client_lock(honch_client_t *client);
static void honch_client_unlock(honch_client_t *client);

static honch_status_t honch_client_enforce_custom_queue_limit(honch_client_t *client)
{
    if (client != NULL && client->queued_event_count < client->max_queued_events) {
        return HONCH_OK;
    }

    if (client == NULL || client->event_queue == NULL ||
        client->event_queue->queue_depth == NULL || client->event_queue->queue_drop_oldest == NULL) {
        return HONCH_OK;
    }

    size_t depth = 0u;
    honch_status_t status = honch_client_queue_depth(client, &depth);
    if (status != HONCH_OK) {
        return status;
    }
    client->queued_event_count = depth;

    while (depth >= client->max_queued_events) {
        status = client->event_queue->queue_drop_oldest(client->event_queue->ctx);
        if (status != HONCH_OK) {
            return status;
        }

        if (depth > 0u) {
            depth--;
        }
        size_t refreshed_depth = 0u;
        if (honch_client_queue_depth(client, &refreshed_depth) == HONCH_OK && refreshed_depth < depth) {
            depth = refreshed_depth;
        }
        client->queued_event_count = depth;
    }

    return HONCH_OK;
}

static honch_status_t honch_build_event(
    honch_client_t *client,
    const char *event_name,
    const honch_wire_v2_property_t *user_properties,
    size_t user_property_count,
    const honch_wire_v2_property_t *trusted_properties,
    size_t trusted_property_count,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_payload_t *out)
{
    out->data = NULL;
    out->length = 0u;

    size_t property_count = 0u;
    honch_status_t status = honch_build_property_pairs(
        client->build_properties,
        &property_count,
        user_properties,
        user_property_count,
        trusted_properties,
        trusted_property_count,
        battery_level,
        auto_properties);
    if (status == HONCH_OK) {
        status = honch_event_record_build(
            event_name,
            client->distinct_id,
            client->session_id,
            honch_client_event_timestamp_millis(client),
            client->build_properties,
            property_count,
            out);
    }
    if (status == HONCH_OK && out->length > client->max_event_bytes) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (status != HONCH_OK) {
        free(out->data);
        out->data = NULL;
        out->length = 0u;
    }
    return status;
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

static uint64_t honch_client_event_timestamp_millis(honch_client_t *client)
{
    uint64_t now_ms = honch_client_now_millis(client);
    return now_ms == 0u ? 1u : now_ms;
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

    if (client != NULL && client->event_queue != NULL && client->event_queue->queue_push != NULL) {
        honch_status_t status = honch_client_enforce_custom_queue_limit(client);
        if (status != HONCH_OK) {
            return status;
        }

        uint64_t sequence = client->sequence;
        status = client->event_queue->queue_push(client->event_queue->ctx, event, event_size, sequence);
        if (status == HONCH_OK) {
            client->sequence++;
            if (sequence_out != NULL) {
                *sequence_out = sequence;
            }
            if (client->queued_event_count < SIZE_MAX) {
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
    if (client != NULL && client->event_queue != NULL && client->event_queue->queue_consume != NULL) {
        return client->event_queue->queue_consume(client->event_queue->ctx, sequence);
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
    if (client != NULL && client->event_queue != NULL && client->event_queue->queue_depth != NULL) {
        return client->event_queue->queue_depth(client->event_queue->ctx, depth);
    }

    return honch_queue_count_pending(client, depth);
}

static honch_status_t honch_core_sync_sequence_from_storage(honch_client_t *client, size_t depth)
{
    if (client == NULL || depth == 0u || client->event_queue == NULL || client->event_queue->queue_peek == NULL) {
        return HONCH_OK;
    }

    uint64_t max_sequence = 0u;
    bool saw_sequence = false;
    for (size_t i = 0u; i < depth; i++) {
        honch_storage_reader_t reader = {0};
        honch_status_t status = client->event_queue->queue_peek(client->event_queue->ctx, &reader);
        if (status == HONCH_ERROR_NOT_INITIALIZED) {
            break;
        }
        if (status != HONCH_OK) {
            return status;
        }
        if (!saw_sequence || reader.sequence > max_sequence) {
            max_sequence = reader.sequence;
            saw_sequence = true;
        }
    }

    if (!saw_sequence) {
        return HONCH_OK;
    }
    if (max_sequence == UINT64_MAX) {
        return HONCH_ERROR_QUEUE_FULL;
    }
    if (client->sequence <= max_sequence) {
        client->sequence = max_sequence + 1u;
    }
    return HONCH_OK;
}

static honch_status_t honch_client_queue_clear(honch_client_t *client)
{
    if (client != NULL && client->event_queue != NULL && client->event_queue->queue_clear != NULL) {
        return client->event_queue->queue_clear(client->event_queue->ctx);
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
    const honch_wire_v2_property_t *properties,
    size_t property_count,
    const honch_wire_v2_property_t *trusted_properties,
    size_t trusted_property_count,
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
    return honch_track_locked_internal(
        client,
        "$battery_low",
        (const honch_wire_v2_property_t[]) {
            honch_prop("level", honch_i64(battery_level))
        },
        1u,
        NULL,
        0u,
        battery_level,
        false,
        auto_properties,
        lifecycle_tracker);
}

static honch_status_t honch_track_locked_internal(
    honch_client_t *client,
    const char *event_name,
    const honch_wire_v2_property_t *properties,
    size_t property_count,
    const honch_wire_v2_property_t *trusted_properties,
    size_t trusted_property_count,
    int battery_level,
    bool check_battery_low,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    honch_payload_t event = {0};
    honch_status_t status = honch_build_event(
        client,
        event_name,
        properties,
        property_count,
        trusted_properties,
        trusted_property_count,
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

static bool honch_scheduler_outbound_ready_locked(honch_client_t *client, uint64_t now)
{
    return client->flush_min_interval_ms == 0u || client->next_outbound_flush_ms <= now;
}

static void honch_scheduler_record_outbound_attempt(honch_client_t *client, uint64_t now)
{
    if (client->flush_min_interval_ms == 0u) {
        client->next_outbound_flush_ms = 0u;
        return;
    }

    uint64_t wait_ms = client->flush_min_interval_ms;
    if (UINT64_MAX - now < wait_ms) {
        wait_ms = UINT64_MAX - now;
    }
    client->next_outbound_flush_ms = now + wait_ms;
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

static uint64_t honch_transport_retry_after_ms(honch_client_t *client)
{
    if (client == NULL || client->transport == NULL || client->transport->retry_after_ms == NULL) {
        return 0u;
    }
    return client->transport->retry_after_ms(client->transport->ctx);
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

static void honch_scheduler_record_flush_result(
    honch_client_t *client,
    honch_status_t status,
    uint64_t now,
    bool outbound_upload_attempted)
{
    if (status == HONCH_OK) {
        client->current_retry_delay_ms = client->flush_retry_initial_ms;
        client->next_retry_flush_ms = 0u;
        if (outbound_upload_attempted) {
            honch_scheduler_record_outbound_attempt(client, now);
        }
    } else if (honch_status_is_retryable(status)) {
        uint64_t wait_ms = honch_next_retry_delay_ms(client);
        uint64_t retry_after_ms = honch_transport_retry_after_ms(client);
        if (retry_after_ms > wait_ms) {
            wait_ms = retry_after_ms;
        }
        if (UINT64_MAX - now < wait_ms) {
            wait_ms = UINT64_MAX - now;
        }
        client->next_retry_flush_ms = now + wait_ms;
        client->scheduler_flush_requested = true;
        honch_grow_retry_delay(client);
    }

    if (client->flush_interval_seconds > 0u) {
        client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
    }
}

static bool honch_scheduler_due_locked(honch_client_t *client, uint64_t now)
{
    if (client->next_retry_flush_ms > now) {
        return false;
    }
    if (client->scheduler_flush_requested) {
        return true;
    }
    return client->flush_interval_seconds > 0u && now >= client->next_interval_flush_ms;
}

static honch_status_t honch_scheduler_check_outbound_spacing_locked(
    honch_client_t *client,
    uint64_t now,
    bool *delayed)
{
    if (delayed == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *delayed = false;
    if (honch_scheduler_outbound_ready_locked(client, now)) {
        return HONCH_OK;
    }

    size_t pending_count = 0u;
    honch_status_t status = honch_client_queue_depth(client, &pending_count);
    if (status != HONCH_OK) {
        return status;
    }
    client->queued_event_count = pending_count;
    if (pending_count > 0u) {
        client->scheduler_flush_requested = true;
        *delayed = true;
    } else {
        client->scheduler_flush_requested = false;
    }
    return HONCH_OK;
}

static bool honch_scheduler_connectivity_ready_locked(honch_client_t *client)
{
    if (client == NULL || client->connectivity_callback == NULL) {
        return true;
    }
    return client->connectivity_callback(client->connectivity_userdata) > 0;
}

static honch_status_t honch_scheduler_check_connectivity_locked(honch_client_t *client, bool *offline)
{
    if (offline == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *offline = false;

    size_t pending_count = 0u;
    honch_status_t status = honch_client_queue_depth(client, &pending_count);
    if (status != HONCH_OK) {
        return status;
    }
    client->queued_event_count = pending_count;
    if (pending_count == 0u) {
        client->scheduler_flush_requested = false;
        return HONCH_OK;
    }

    if (!honch_scheduler_connectivity_ready_locked(client)) {
        client->scheduler_flush_requested = true;
        *offline = true;
    }
    return HONCH_OK;
}

static void honch_scheduler_notify_after_enqueue_locked(honch_client_t *client)
{
    if (client == NULL || client->flush_event_threshold == 0u) {
        return;
    }

    if (client->queued_event_count >= client->flush_event_threshold) {
        client->scheduler_flush_requested = true;
    }
}

static void honch_scheduler_refresh_queue_request_locked(honch_client_t *client)
{
    if (client == NULL || client->flush_event_threshold == 0u) {
        return;
    }

    size_t pending_count = 0u;
    honch_status_t status = honch_client_queue_depth(client, &pending_count);
    if (status != HONCH_OK) {
        return;
    }

    client->queued_event_count = pending_count;
    if (pending_count >= client->flush_event_threshold) {
        client->scheduler_flush_requested = true;
    }
}

static void honch_scheduler_start(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

    uint64_t now = honch_client_now_millis(client);
    client->current_retry_delay_ms = client->flush_retry_initial_ms;
    if (client->flush_interval_seconds > 0u) {
        client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
    }

    if (client->flush_event_threshold > 0u) {
        size_t pending_count = 0u;
        honch_status_t status = honch_client_queue_depth(client, &pending_count);
        if (status == HONCH_OK) {
            client->queued_event_count = pending_count;
            client->scheduler_flush_requested = pending_count >= client->flush_event_threshold;
        }
    }
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

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status == HONCH_OK) {
        const honch_wire_v2_property_t properties[] = {
            honch_prop("previous_version", honch_str(previous_version)),
            honch_prop("new_version", honch_str(client->firmware_version))
        };
        status = honch_track_locked_internal(
            client,
            "$firmware_update",
            properties,
            sizeof(properties) / sizeof(properties[0]),
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            lifecycle_tracker);
    }
    honch_event_context_free(&event_context);
    if (status == HONCH_OK) {
        if (firmware_version_pending_save != NULL) {
            *firmware_version_pending_save = true;
        } else {
            status = honch_state_save_firmware_version(client);
        }
    }

    free(previous_version);
    return status;
}

static const char *honch_fault_kind_source(honch_fault_kind_t kind)
{
    switch (kind) {
    case HONCH_FAULT_KIND_NONE:
        return "none";
    case HONCH_FAULT_KIND_PANIC:
        return "panic";
    case HONCH_FAULT_KIND_WATCHDOG:
        return "watchdog";
    case HONCH_FAULT_KIND_ASSERT:
        return "assert";
    case HONCH_FAULT_KIND_BROWNOUT:
        return "brownout";
    case HONCH_FAULT_KIND_STACK_OVERFLOW:
        return "stack_overflow";
    case HONCH_FAULT_KIND_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *honch_fault_severity_string(honch_fault_severity_t severity)
{
    switch (severity) {
    case HONCH_FAULT_SEVERITY_INFO:
        return "info";
    case HONCH_FAULT_SEVERITY_WARNING:
        return "warning";
    case HONCH_FAULT_SEVERITY_FATAL:
        return "fatal";
    default:
        return "fatal";
    }
}

static bool honch_fault_snapshot_is_abnormal(const honch_fault_snapshot_t *fault_snapshot)
{
    return fault_snapshot != NULL && fault_snapshot->kind != HONCH_FAULT_KIND_NONE;
}

static const char *honch_fault_reset_reason(const honch_fault_snapshot_t *fault_snapshot)
{
    if (fault_snapshot == NULL || honch_is_blank(fault_snapshot->reset_reason)) {
        return "unknown";
    }
    return fault_snapshot->reset_reason;
}

static honch_status_t honch_emit_fault_locked(
    honch_client_t *client,
    const honch_fault_snapshot_t *fault_snapshot,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    if (!honch_fault_snapshot_is_abnormal(fault_snapshot)) {
        return HONCH_OK;
    }

    honch_wire_v2_property_t properties[5];
    size_t property_count = 0u;
    honch_status_t status = honch_append_typed_property(
        properties,
        &property_count,
        "source",
        honch_str(honch_fault_kind_source(fault_snapshot->kind)),
        true);
    if (status == HONCH_OK) {
        status = honch_append_typed_property(
            properties,
            &property_count,
            "severity",
            honch_str(honch_fault_severity_string(fault_snapshot->severity)),
            true);
    }
    if (status == HONCH_OK) {
        status = honch_append_typed_property(
            properties,
            &property_count,
            "reset_reason",
            honch_str(honch_fault_reset_reason(fault_snapshot)),
            true);
    }
    if (status == HONCH_OK && !honch_is_blank(fault_snapshot->message)) {
        status = honch_append_typed_property(
            properties,
            &property_count,
            "message",
            honch_str(fault_snapshot->message),
            true);
    }
    if (status == HONCH_OK && !honch_is_blank(fault_snapshot->component)) {
        status = honch_append_typed_property(
            properties,
            &property_count,
            "component",
            honch_str(fault_snapshot->component),
            true);
    }
    if (status != HONCH_OK) {
        return HONCH_OK;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status == HONCH_OK) {
        status = honch_track_locked_internal(
            client,
            "$error",
            properties,
            property_count,
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            lifecycle_tracker);
    }
    honch_event_context_free(&event_context);
    return HONCH_OK;
}

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config)
{
    if (client == NULL || config == NULL || honch_is_blank(config->api_key) ||
        honch_is_blank(config->endpoint_url) || honch_is_blank(config->device_model) ||
        honch_is_blank(config->firmware_version)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *client = NULL;

    honch_client_t *next = (honch_client_t *)calloc(1u, sizeof(*next));
    if (next == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0u; i < HONCH_AUTO_PROPERTY_BUFFER_COUNT; i++) {
        honch_atomic_bool_init(&next->auto_property_buffer_in_use[i], false);
    }
    if (config->platform != NULL) {
        next->platform_ops = *config->platform;
        if (next->platform_ops.ctx == NULL) {
            next->platform_ops.ctx = next;
        }
        next->platform = &next->platform_ops;
    }
    if (config->state_storage != NULL) {
        next->state_storage_ops = *config->state_storage;
        if (next->state_storage_ops.ctx == NULL) {
            next->state_storage_ops.ctx = next;
        }
        next->state_storage = &next->state_storage_ops;
    }
    if (config->event_queue != NULL) {
        next->event_queue_ops = *config->event_queue;
        if (next->event_queue_ops.ctx == NULL) {
            next->event_queue_ops.ctx = next;
        }
        next->event_queue = &next->event_queue_ops;
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
    next->queue_directory = honch_strdup(honch_is_blank(config->queue_directory) ? "" : config->queue_directory);
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
    next->flush_min_interval_ms = config->flush_min_interval_ms == HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS ?
        0u :
        (config->flush_min_interval_ms == 0u ? HONCH_DEFAULT_FLUSH_MIN_INTERVAL_MS : config->flush_min_interval_ms);
    next->flush_event_threshold = config->flush_event_threshold == 0u ?
        HONCH_DEFAULT_FLUSH_EVENT_THRESHOLD :
        config->flush_event_threshold;
    next->flush_max_batches = config->flush_max_batches == 0u ?
        HONCH_DEFAULT_FLUSH_MAX_BATCHES :
        config->flush_max_batches;
    next->shutdown_flush_max_batches = config->shutdown_flush_max_batches == 0u ?
        HONCH_DEFAULT_SHUTDOWN_FLUSH_MAX_BATCHES :
        config->shutdown_flush_max_batches;
    next->flush_retry_initial_ms = config->flush_retry_initial_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS :
        config->flush_retry_initial_ms;
    next->flush_retry_max_ms = config->flush_retry_max_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_MAX_MS :
        config->flush_retry_max_ms;
    if (next->flush_retry_max_ms < next->flush_retry_initial_ms) {
        next->flush_retry_max_ms = next->flush_retry_initial_ms;
    }
    next->durability_mode =
        config->durability_mode == HONCH_DURABILITY_SYNC_ALWAYS ? HONCH_DURABILITY_SYNC_ALWAYS : HONCH_DURABILITY_OS_BUFFERED;
    next->battery_callback = config->battery_callback;
    next->battery_low_threshold = config->battery_low_threshold > 0 ?
        config->battery_low_threshold :
        HONCH_DEFAULT_BATTERY_LOW_THRESHOLD;
    next->auto_properties_callback = config->auto_properties_callback;
    next->auto_properties_userdata = config->auto_properties_userdata;
    next->connectivity_callback = config->connectivity_callback;
    next->connectivity_userdata = config->connectivity_userdata;

    honch_status_t status = HONCH_OK;
    bool lifetime_mutex_initialized = false;
    bool state_mutex_initialized = false;
    bool firmware_version_pending_save = false;
    honch_lifecycle_queue_tracker_t lifecycle_tracker = {0};
    if (next->api_key == NULL || next->endpoint_url == NULL || next->queue_directory == NULL ||
        next->sdk_platform == NULL) {
        status = HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (status == HONCH_OK && !honch_client_lock_ops_valid(next->platform)) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (status == HONCH_OK) {
        status = honch_client_lock_create(next, &next->lifetime_mutex);
        lifetime_mutex_initialized = status == HONCH_OK;
    }
    if (status == HONCH_OK) {
        status = honch_client_lock_create(next, &next->state_mutex);
        state_mutex_initialized = status == HONCH_OK;
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
    if (status == HONCH_OK) {
        status = honch_core_sync_sequence_from_storage(next, next->queued_event_count);
    }
    if (status == HONCH_OK) {
        honch_lifecycle_queue_tracker_begin(next, &lifecycle_tracker);
        status = honch_emit_firmware_update_locked(next, &lifecycle_tracker, &firmware_version_pending_save);
    }
    if (status == HONCH_OK) {
        honch_event_context_t event_context = {.battery_level = -1};
        status = honch_prepare_event_context(next, &event_context);
        if (status == HONCH_OK) {
            const honch_wire_v2_property_t boot_properties[] = {
                honch_prop("reset_reason", honch_str(honch_fault_reset_reason(config->fault_snapshot)))
            };
            status = honch_track_locked_internal(
                next,
                "$device_boot",
                boot_properties,
                sizeof(boot_properties) / sizeof(boot_properties[0]),
                NULL,
                0u,
                event_context.battery_level,
                true,
                &event_context.auto_properties,
                &lifecycle_tracker);
        }
        honch_event_context_free(&event_context);
    }
    if (status == HONCH_OK) {
        status = honch_emit_fault_locked(next, config->fault_snapshot, &lifecycle_tracker);
    }
    if (status == HONCH_OK && firmware_version_pending_save) {
        status = honch_state_save_firmware_version(next);
    }
    if (status == HONCH_OK) {
        honch_scheduler_start(next);
    }

    if (status != HONCH_OK) {
        if (lifecycle_tracker.count > 0u) {
            honch_status_t rollback_status = honch_lifecycle_queue_tracker_rollback(next, &lifecycle_tracker);
            if (rollback_status != HONCH_OK) {
                status = rollback_status;
            }
        }
        if (state_mutex_initialized) {
            honch_client_lock_destroy(next, next->state_mutex);
        }
        if (lifetime_mutex_initialized) {
            honch_client_lock_destroy(next, next->lifetime_mutex);
        }
        honch_free_client_fields(next);
        free(next);
        return status;
    }

    *client = next;
    return HONCH_OK;
}

honch_status_t honch_core_track(
    honch_client_t *client,
    const char *event_name,
    const honch_wire_v2_property_t *properties,
    size_t property_count)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_event_name(event_name) != HONCH_OK ||
        (property_count > 0u && properties == NULL) ||
        property_count > HONCH_MAX_EVENT_PROPERTIES) {
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
        properties,
        property_count,
        NULL,
        0u,
        event_context.battery_level,
        true,
        &event_context.auto_properties,
        NULL);
    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_identify(
    honch_client_t *client,
    const char *distinct_id,
    const honch_wire_v2_property_t *traits,
    size_t trait_count)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_distinct_id(distinct_id) != HONCH_OK ||
        (trait_count > 0u && traits == NULL) ||
        trait_count >= HONCH_MAX_EVENT_PROPERTIES) {
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

    const honch_wire_v2_property_t identify_properties[] = {
        honch_prop("$anon_distinct_id", honch_str(previous_distinct_id))
    };
    status = honch_track_locked_internal(
        client,
        "$identify",
        traits,
        trait_count,
        identify_properties,
        sizeof(identify_properties) / sizeof(identify_properties[0]),
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

honch_status_t honch_core_set_property(honch_client_t *client, const char *key, honch_wire_v2_value_t value)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (honch_validate_user_property_key(key) != HONCH_OK) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status == HONCH_OK) {
        status = honch_client_lock(client);
        if (status == HONCH_OK) {
            const honch_wire_v2_property_t properties[] = {
                honch_prop(key, value)
            };
            status = honch_track_locked_internal(
                client,
                "$set_property",
                properties,
                1u,
                NULL,
                0u,
                event_context.battery_level,
                true,
                &event_context.auto_properties,
                NULL);
            honch_client_unlock(client);
        }
    }

    honch_event_context_free(&event_context);
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

    honch_event_context_t end_event_context = {.battery_level = -1};
    honch_event_context_t start_event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &end_event_context);
    if (status == HONCH_OK) {
        status = honch_prepare_event_context(client, &start_event_context);
    }
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    honch_lifecycle_queue_tracker_t replacement_tracker;
    honch_lifecycle_queue_tracker_begin(client, &replacement_tracker);
    char *old_session_id = NULL;
    bool replacing_session = client->session_id != NULL;

    if (replacing_session) {
        status = honch_track_locked_internal(
            client,
            "$session_end",
            NULL,
            0u,
            NULL,
            0u,
            end_event_context.battery_level,
            true,
            &end_event_context.auto_properties,
            &replacement_tracker);
        if (status == HONCH_OK) {
            old_session_id = client->session_id;
            client->session_id = NULL;
        }
    }
    if (status == HONCH_OK) {
        client->session_id = session_id;
        session_id = NULL;
        const honch_wire_v2_property_t *start_properties = NULL;
        size_t start_property_count = 0u;
        const honch_wire_v2_property_t named_start_properties[] = {
            honch_prop("session_name", honch_str(session_name))
        };
        if (!honch_is_blank(session_name)) {
            start_properties = named_start_properties;
            start_property_count = 1u;
        }
        status = honch_track_locked_internal(
            client,
            "$session_start",
            start_properties,
            start_property_count,
            NULL,
            0u,
            start_event_context.battery_level,
            true,
            &start_event_context.auto_properties,
            &replacement_tracker);
        if (status != HONCH_OK) {
            free(client->session_id);
            client->session_id = old_session_id;
            old_session_id = NULL;
            if (replacing_session) {
                honch_status_t rollback_status =
                    honch_lifecycle_queue_tracker_rollback(client, &replacement_tracker);
                if (rollback_status != HONCH_OK) {
                    status = rollback_status;
                }
            }
        }
    }
    if (status == HONCH_OK) {
        free(old_session_id);
    }

    honch_client_unlock(client);
    honch_event_context_free(&end_event_context);
    honch_event_context_free(&start_event_context);
    free(session_id);
    free(old_session_id);
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
            0u,
            NULL,
            0u,
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

honch_status_t honch_core_tick(honch_client_t *client)
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

    uint64_t now = honch_client_now_millis(client);
    if (!honch_scheduler_due_locked(client, now)) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return HONCH_OK;
    }
    bool delayed = false;
    status = honch_scheduler_check_outbound_spacing_locked(client, now, &delayed);
    if (status != HONCH_OK || delayed) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return status == HONCH_OK ? HONCH_OK : status;
    }
    bool offline = false;
    status = honch_scheduler_check_connectivity_locked(client, &offline);
    if (status != HONCH_OK || offline) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return status == HONCH_OK ? HONCH_OK : status;
    }
    if (client->flush_in_progress) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return HONCH_ERROR_BUSY;
    }

    client->flush_in_progress = true;
    client->outbound_upload_attempted = false;
    client->scheduler_flush_requested = false;
    bool progressed = false;
    status = honch_queue_flush_one_chunk_locked(client, &progressed);
    now = honch_client_now_millis(client);
    if (status == HONCH_ERROR_REJECTED && progressed) {
        status = HONCH_OK;
    }
    honch_scheduler_record_flush_result(client, status, now, client->outbound_upload_attempted);
    client->outbound_upload_attempted = false;
    if (status == HONCH_OK) {
        honch_scheduler_refresh_queue_request_locked(client);
    }
    client->flush_in_progress = false;
    honch_client_unlock(client);
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
    if (client->flush_in_progress) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return HONCH_ERROR_BUSY;
    }
    bool offline = false;
    status = honch_scheduler_check_connectivity_locked(client, &offline);
    if (status != HONCH_OK || offline) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return status == HONCH_OK ? HONCH_ERROR_OFFLINE : status;
    }
    uint64_t now = honch_client_now_millis(client);
    bool delayed = false;
    status = honch_scheduler_check_outbound_spacing_locked(client, now, &delayed);
    if (status != HONCH_OK || delayed) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return status == HONCH_OK ? HONCH_ERROR_RATE_LIMITED : status;
    }

    client->flush_in_progress = true;
    client->outbound_upload_attempted = false;
    status = honch_queue_flush_limited_locked(client, client->flush_max_batches);
    now = honch_client_now_millis(client);
    honch_scheduler_record_flush_result(client, status, now, client->outbound_upload_attempted);
    client->outbound_upload_attempted = false;
    client->flush_in_progress = false;
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
    if (client->flush_in_progress) {
        honch_client_unlock(client);
        honch_client_leave(client);
        return HONCH_ERROR_BUSY;
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

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        honch_client_lock_destroy(client, client->state_mutex);
        honch_client_lock_destroy(client, client->lifetime_mutex);
        honch_free_client_fields(client);
        free(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        honch_client_lock_destroy(client, client->state_mutex);
        honch_client_lock_destroy(client, client->lifetime_mutex);
        honch_free_client_fields(client);
        free(client);
        return status;
    }

    status = honch_track_locked_internal(
        client,
        "$device_shutdown",
        NULL,
        0u,
        NULL,
        0u,
        event_context.battery_level,
        true,
        &event_context.auto_properties,
        NULL);
    honch_status_t flush_status = honch_queue_flush_limited_locked(client, client->shutdown_flush_max_batches);
    if (status == HONCH_OK) {
        status = flush_status;
    }
    honch_client_unlock(client);
    honch_event_context_free(&event_context);

    honch_client_lock_destroy(client, client->state_mutex);
    honch_client_lock_destroy(client, client->lifetime_mutex);
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

honch_status_t honch_core_get_queue_stats(honch_client_t *client, honch_queue_stats_t *stats)
{
    if (client == NULL || stats == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (client->event_queue == NULL || client->event_queue->queue_get_stats == NULL) {
        return HONCH_ERROR_NOT_SUPPORTED;
    }
    return client->event_queue->queue_get_stats(client->event_queue->ctx, stats);
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
        case HONCH_ERROR_BUSY:
            return "busy";
        case HONCH_ERROR_NOT_SUPPORTED:
            return "not supported";
        case HONCH_ERROR_OFFLINE:
            return "offline";
        default:
            return "unknown";
    }
}
