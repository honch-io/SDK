#include "honch_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FAKE_STORAGE_MAX_EVENTS 64u

void honch_test_reset_wire_v2_encode_attempts(void);
size_t honch_test_max_wire_v2_encode_attempts(void);

typedef struct fake_event {
    uint8_t *data;
    size_t size;
    uint64_t sequence;
    bool pending;
    bool consumed;
    bool dead_lettered;
} fake_event_t;

typedef struct fake_storage {
    fake_event_t events[FAKE_STORAGE_MAX_EVENTS];
    size_t event_count;
    size_t peek_index;
    size_t peek_calls;
    size_t read_batch_calls;
    size_t consume_batch_calls;
} fake_storage_t;

typedef struct fake_transport {
    honch_transport_result_t result;
    honch_status_t status;
    size_t calls;
    size_t chunk_calls;
    char last_stream_id[32];
    uint64_t last_message_id;
    bool reject_missing_stream_id;
    uint8_t last_chunk[512];
    size_t last_chunk_size;
} fake_transport_t;

static fake_event_t *find_event(fake_storage_t *storage, uint64_t sequence)
{
    for (size_t i = 0u; i < storage->event_count; i++) {
        if (storage->events[i].sequence == sequence) {
            return &storage->events[i];
        }
    }
    return NULL;
}

static honch_status_t fake_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    fake_event_t *event = (fake_event_t *)ctx;
    if (event == NULL || buffer == NULL || offset > event->size || buffer_size > event->size - offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    memcpy(buffer, event->data + offset, buffer_size);
    return HONCH_OK;
}

static honch_status_t fake_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    storage->peek_calls++;
    fake_event_t *event = NULL;
    for (size_t i = storage->peek_index; i < storage->event_count; i++) {
        if (storage->events[i].pending) {
            event = &storage->events[i];
            storage->peek_index = i + 1u;
            break;
        }
    }
    if (event == NULL) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    *reader = (honch_storage_reader_t) {
        .ctx = event,
        .read = fake_reader_read,
        .total_size = event->size,
        .sequence = event->sequence
    };
    return HONCH_OK;
}

static honch_status_t fake_queue_read_batch(
    void *ctx,
    honch_storage_event_t *events,
    size_t max_events,
    size_t max_event_bytes,
    size_t *event_count)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    storage->read_batch_calls++;
    *event_count = 0u;
    for (size_t i = 0u; i < storage->event_count && *event_count < max_events; i++) {
        fake_event_t *event = &storage->events[i];
        if (!event->pending) {
            continue;
        }
        if (event->size == 0u || event->size > max_event_bytes) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }

        uint8_t *copy = (uint8_t *)malloc(event->size);
        assert(copy != NULL);
        memcpy(copy, event->data, event->size);
        events[*event_count] = (honch_storage_event_t) {
            .data = copy,
            .length = event->size,
            .sequence = event->sequence
        };
        (*event_count)++;
    }
    return *event_count == 0u ? HONCH_ERROR_NOT_INITIALIZED : HONCH_OK;
}

static honch_status_t fake_queue_read_batch_overreports(
    void *ctx,
    honch_storage_event_t *events,
    size_t max_events,
    size_t max_event_bytes,
    size_t *event_count)
{
    (void)ctx;
    (void)events;
    (void)max_event_bytes;
    *event_count = max_events + 1u;
    return HONCH_OK;
}

static honch_status_t fake_queue_consume(void *ctx, uint64_t sequence)
{
    fake_event_t *event = find_event((fake_storage_t *)ctx, sequence);
    if (event == NULL || !event->pending) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    event->pending = false;
    event->consumed = true;
    return HONCH_OK;
}

static honch_status_t fake_queue_consume_batch(void *ctx, const uint64_t *sequences, size_t sequence_count)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    storage->consume_batch_calls++;
    for (size_t i = 0u; i < sequence_count; i++) {
        honch_status_t status = fake_queue_consume(ctx, sequences[i]);
        if (status != HONCH_OK) {
            return status;
        }
    }
    return HONCH_OK;
}

static honch_status_t fake_queue_dead_letter(void *ctx, uint64_t sequence)
{
    fake_event_t *event = find_event((fake_storage_t *)ctx, sequence);
    if (event == NULL || !event->pending) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    event->pending = false;
    event->dead_lettered = true;
    return HONCH_OK;
}

static honch_status_t fake_queue_depth(void *ctx, size_t *depth)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    storage->peek_index = 0u;
    *depth = 0u;
    for (size_t i = 0u; i < storage->event_count; i++) {
        if (storage->events[i].pending) {
            (*depth)++;
        }
    }
    return HONCH_OK;
}

static void build_heavy_event(
    const char *event_name,
    size_t property_count,
    size_t array_length,
    uint8_t **out_data,
    size_t *out_size)
{
    assert(out_data != NULL);
    assert(out_size != NULL);

    honch_buffer_t properties = {0};
    assert(honch_buffer_init(&properties, (property_count * array_length * 4u) + 256u) == HONCH_OK);
    size_t member_count = 0u;
    for (size_t i = 0u; i < property_count; i++) {
        char key[8];
        int key_length = snprintf(key, sizeof(key), "p%02zu", i);
        assert(key_length > 0 && (size_t)key_length < sizeof(key));
        honch_buffer_t array = {0};
        assert(honch_buffer_init(&array, (array_length * 4u) + 8u) == HONCH_OK);
        assert(honch_buffer_append(&array, "[") == HONCH_OK);
        for (size_t j = 0u; j < array_length; j++) {
            if (j > 0u) {
                assert(honch_buffer_append(&array, ",") == HONCH_OK);
            }
            assert(honch_buffer_appendf(&array, "%zu", i + j) == HONCH_OK);
        }
        assert(honch_buffer_append(&array, "]") == HONCH_OK);
        assert(honch_event_record_append_property_json(&properties, &member_count, key, array.data) == HONCH_OK);
        honch_buffer_free(&array);
    }

    honch_payload_t payload = {0};
    assert(honch_event_record_build(event_name, "device-1", NULL, 1234u, &properties, member_count, &payload) ==
        HONCH_OK);
    honch_buffer_free(&properties);
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
    (void)endpoint_url;
    (void)api_key;
    fake_transport_t *transport = (fake_transport_t *)ctx;
    transport->chunk_calls++;
    snprintf(transport->last_stream_id, sizeof(transport->last_stream_id), "%s", stream_id == NULL ? "" : stream_id);
    if (transport->reject_missing_stream_id && stream_id == NULL) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    transport->last_message_id = 0u;
    unsigned int shift = 0u;
    for (size_t i = 1u; i < body_size && i <= 10u; i++) {
        transport->last_message_id |= (uint64_t)(body[i] & 0x7fu) << shift;
        if ((body[i] & 0x80u) == 0u) {
            break;
        }
        shift += 7u;
    }
    transport->last_chunk_size = body_size < sizeof(transport->last_chunk) ? body_size : sizeof(transport->last_chunk);
    memcpy(transport->last_chunk, body, transport->last_chunk_size);
    *result = transport->result;
    return transport->status;
}

static bool bytes_contains(const uint8_t *haystack, size_t haystack_size, const char *needle, size_t needle_size)
{
    if (needle_size == 0u || haystack_size < needle_size) {
        return false;
    }

    for (size_t i = 0u; i <= haystack_size - needle_size; i++) {
        if (memcmp(haystack + i, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

static honch_payload_t build_test_event_for_distinct(
    const char *event_name,
    const char *distinct_id,
    const char *properties_json)
{
    honch_buffer_t properties = {0};
    assert(honch_buffer_init(&properties, 128u) == HONCH_OK);
    size_t property_count = 0u;
    assert(honch_event_record_append_json_object_members(&properties, properties_json, &property_count) == HONCH_OK);
    honch_payload_t payload = {0};
    assert(honch_event_record_build(event_name, distinct_id, NULL, 1234u, &properties, property_count, &payload) ==
        HONCH_OK);
    honch_buffer_free(&properties);
    return payload;
}

static honch_payload_t build_test_event(const char *event_name, const char *properties_json)
{
    return build_test_event_for_distinct(event_name, "device-1", properties_json);
}

static uint8_t *find_payload_bytes(honch_payload_t *payload, const char *needle)
{
    size_t needle_length = strlen(needle);
    for (size_t i = 0u; i + needle_length <= payload->length; i++) {
        if (memcmp(payload->data + i, needle, needle_length) == 0) {
            return payload->data + i;
        }
    }
    return NULL;
}

static void test_hqr1_validate_rejects_embedded_nul_in_required_strings(void)
{
    honch_payload_t event = build_test_event("clickx", "{\"modex\":\"on\"}");

    uint8_t *event_name = find_payload_bytes(&event, "clickx");
    assert(event_name != NULL);
    event_name[5] = '\0';
    assert(!honch_event_record_validate(event.data, event.length));

    event_name[5] = 'x';
    uint8_t *property_key = find_payload_bytes(&event, "modex");
    assert(property_key != NULL);
    property_key[4] = '\0';
    assert(!honch_event_record_validate(event.data, event.length));

    free(event.data);
}

static void test_hqr1_rejects_values_deeper_than_wire_v2_limit(void)
{
    honch_buffer_t properties = {0};
    assert(honch_buffer_init(&properties, 128u) == HONCH_OK);
    size_t property_count = 0u;
    assert(honch_event_record_append_property_json(&properties, &property_count, "allowed", "[[[[[[[[1]]]]]]]]") ==
        HONCH_OK);
    assert(property_count == 1u);
    honch_buffer_free(&properties);

    assert(honch_buffer_init(&properties, 128u) == HONCH_OK);
    property_count = 0u;
    assert(honch_event_record_append_property_json(&properties, &property_count, "nested", "[[[[[[[[[1]]]]]]]]]") ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(property_count == 0u);
    honch_buffer_free(&properties);
}

static void test_hqr1_rejects_duplicate_top_level_property_keys(void)
{
    honch_buffer_t properties = {0};
    assert(honch_buffer_init(&properties, 128u) == HONCH_OK);
    size_t property_count = 0u;
    assert(honch_event_record_append_json_object_members(&properties, "{\"mode\":\"a\",\"mode\":\"b\"}", &property_count) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(property_count == 0u);
    honch_buffer_free(&properties);
}

static void test_hqr1_rejects_duplicate_nested_map_keys(void)
{
    honch_buffer_t properties = {0};
    assert(honch_buffer_init(&properties, 128u) == HONCH_OK);
    size_t property_count = 0u;
    assert(honch_event_record_append_property_json(&properties, &property_count, "nested", "{\"mode\":\"a\",\"mode\":\"b\"}") ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(property_count == 0u);
    honch_buffer_free(&properties);
}

static honch_client_t fake_client(fake_storage_t *storage, honch_storage_ops_t *storage_ops, fake_transport_t *transport, honch_transport_ops_t *transport_ops)
{
    *storage_ops = (honch_storage_ops_t) {
        .queue_peek = fake_queue_peek,
        .queue_read_batch = fake_queue_read_batch,
        .queue_consume = fake_queue_consume,
        .queue_consume_batch = fake_queue_consume_batch,
        .queue_dead_letter = fake_queue_dead_letter,
        .queue_depth = fake_queue_depth,
        .ctx = storage
    };
    *transport_ops = (honch_transport_ops_t) {
        .post_chunk = fake_post_chunk,
        .ctx = transport
    };

    honch_client_t client = {0};
    client.api_key = "test-key";
    client.endpoint_url = "http://collector.local/";
    client.device_id = "device-1";
    client.device_model = "model-x";
    client.firmware_version = "1.0.0";
    client.environment = "test";
    client.sdk_platform = "posix";
    client.wire_v2_message_id_seed = 0x100u;
    snprintf(client.wire_v2_stream_id, sizeof(client.wire_v2_stream_id), "boot0001");
    client.batch_size = 2u;
    client.max_event_bytes = 1024u;
    client.storage = storage_ops;
    client.transport = transport_ops;
    return client;
}

static void setup_storage(fake_storage_t *storage)
{
    honch_payload_t first_event = build_test_event("first", "{}");
    honch_payload_t second_event = build_test_event("second", "{}");
    memset(storage, 0, sizeof(*storage));
    storage->event_count = 2u;
    storage->events[0] = (fake_event_t) {
        .data = first_event.data,
        .size = first_event.length,
        .sequence = 1u,
        .pending = true
    };
    storage->events[1] = (fake_event_t) {
        .data = second_event.data,
        .size = second_event.length,
        .sequence = 2u,
        .pending = true
    };
}

static void test_v2_chunk_transport_consumes_events_on_acceptance(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(transport.last_chunk_size > 4u);
    assert(transport.last_chunk[0] == 0x02u);
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_splits_oversized_batch_without_dead_lettering_first_event(void)
{
    uint8_t *first_event = NULL;
    uint8_t *second_event = NULL;
    size_t first_size = 0u;
    size_t second_size = 0u;
    build_heavy_event("first", 64u, 20u, &first_event, &first_size);
    build_heavy_event("second", 64u, 20u, &second_event, &second_size);

    fake_storage_t storage = {0};
    storage.event_count = 2u;
    storage.events[0] = (fake_event_t) {
        .data = first_event,
        .size = first_size,
        .sequence = 1u,
        .pending = true
    };
    storage.events[1] = (fake_event_t) {
        .data = second_event,
        .size = second_size,
        .sequence = 2u,
        .pending = true
    };

    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    client.max_event_bytes = 8192u;
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.chunk_calls == 2u);
    assert(storage.events[0].consumed);
    assert(storage.events[1].consumed);
    assert(!storage.events[0].dead_lettered);
    assert(!storage.events[1].dead_lettered);

    free(first_event);
    free(second_event);
}

static void test_v2_batch_shrink_does_not_retry_linearly(void)
{
    fake_storage_t storage = {0};
    enum { event_count = 12u };
    uint8_t *events[event_count];
    memset(events, 0, sizeof(events));

    storage.event_count = event_count;
    for (size_t i = 0u; i < event_count; i++) {
        char event_name[16];
        int written = snprintf(event_name, sizeof(event_name), "heavy_%02zu", i);
        assert(written > 0 && (size_t)written < sizeof(event_name));
        size_t event_size = 0u;
        build_heavy_event(event_name, 64u, 20u, &events[i], &event_size);
        storage.events[i] = (fake_event_t) {
            .data = events[i],
            .size = event_size,
            .sequence = i + 1u,
            .pending = true
        };
    }

    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    client.batch_size = event_count;
    client.max_event_bytes = 8192u;
    transport_ops.post_chunk = fake_post_chunk;

    honch_test_reset_wire_v2_encode_attempts();
    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(honch_test_max_wire_v2_encode_attempts() <= 5u);

    for (size_t i = 0u; i < event_count; i++) {
        assert(storage.events[i].consumed);
        free(events[i]);
    }
}

static void test_v2_chunk_transport_preserves_string_properties(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_property = build_test_event("first", "{\"mode\":\"auto\"}");
    storage.events[0].data = event_with_property.data;
    storage.events[0].size = event_with_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "mode", 4u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "auto", 4u));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_flush_borrows_string_property_values(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_property = build_test_event("event", "{\"mode\":\"hdr\"}");
    storage.events[0].data = event_with_property.data;
    storage.events[0].size = event_with_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.chunk_calls == 1u);
}

static void test_v2_flush_accepts_queued_empty_string_property_value(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_empty_string_property = build_test_event("event", "{\"empty\":\"\",\"x\":null}");
    storage.events[0].data = event_with_empty_string_property.data;
    storage.events[0].size = event_with_empty_string_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_nested_properties(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_nested_properties =
        build_test_event("first", "{\"tags\":[\"red\",\"blue\"],\"traits\":{\"tier\":\"gold\"}}");
    storage.events[0].data = event_with_nested_properties.data;
    storage.events[0].size = event_with_nested_properties.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "tags", 4u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "red", 3u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "blue", 4u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "traits", 6u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "tier", 4u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "gold", 4u));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_float64_properties(void)
{
    static const uint8_t expected_float64_value[] = {
        0x06u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x42u, 0x40u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_float_property = build_test_event("first", "{\"temp\":36.5}");
    storage.events[0].data = event_with_float_property.data;
    storage.events[0].size = event_with_float_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "temp", 4u));
    assert(bytes_contains(
        transport.last_chunk,
        transport.last_chunk_size,
        (const char *)expected_float64_value,
        sizeof(expected_float64_value)));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_float32_bool_and_null_properties(void)
{
    static const uint8_t expected_true_value[] = {
        0x11u, 0x02u
    };
    static const uint8_t expected_null_value[] = {
        0x13u, 0x00u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_scalar_properties =
        build_test_event("first", "{\"temp\":36.5,\"active\":true,\"empty\":null}");
    storage.events[0].data = event_with_scalar_properties.data;
    storage.events[0].size = event_with_scalar_properties.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "temp", 4u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "temp", 4u));
    assert(bytes_contains(
        transport.last_chunk,
        transport.last_chunk_size,
        (const char *)expected_true_value,
        sizeof(expected_true_value)));
    assert(bytes_contains(
        transport.last_chunk,
        transport.last_chunk_size,
        (const char *)expected_null_value,
        sizeof(expected_null_value)));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_half_float_as_float32_property(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t event_with_float_property = build_test_event("first", "{\"temp\":1.5}");
    storage.events[0].data = event_with_float_property.data;
    storage.events[0].size = event_with_float_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "temp", 4u));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_flush_rejects_non_record_payload(void)
{
    static const uint8_t invalid_payload[] = {0xa0u};

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)invalid_payload;
    storage.events[0].size = sizeof(invalid_payload);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 0u);
    assert(!storage.events[0].consumed);
    assert(storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_device_boot_event(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t boot_event = build_test_event("$device_boot", "{\"reset_reason\":\"unknown\"}");
    storage.events[0].data = boot_event.data;
    storage.events[0].size = boot_event.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "$device_boot", 12u));
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "unknown", 7u));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_final_chunk_stored_response_preserves_event(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_CHUNK_STORED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(storage.events[0].pending);
    assert(!storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_multi_frame_flush_passes_stream_id_to_transport(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_CHUNK_STORED,
        .status = HONCH_OK,
        .reject_missing_stream_id = true
    };
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(strcmp(transport.last_stream_id, "boot0001") == 0);
    assert(transport.last_message_id == 0x101u);
    assert(storage.events[0].pending);
    assert(!storage.events[0].consumed);
}

static void test_flush_uses_storage_batch_read_when_available(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);

    assert(honch_queue_flush_locked(&client) == HONCH_OK);

    assert(storage.read_batch_calls == 1u);
    assert(storage.peek_calls == 0u);
    assert(storage.consume_batch_calls == 1u);
    assert(storage.events[0].consumed);
    assert(storage.events[1].consumed);
}

static void test_flush_rejects_overreported_storage_batch_count(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    storage_ops.queue_read_batch = fake_queue_read_batch_overreports;

    honch_status_t status = honch_queue_flush_locked(&client);
    if (status != HONCH_ERROR_INTERNAL) {
        fprintf(stderr, "overreported batch status=%d\n", status);
        abort();
    }
    assert(transport.chunk_calls == 0u);
    assert(!storage.events[0].consumed);
    assert(!storage.events[1].consumed);
}

static void test_v2_flush_splits_batches_by_distinct_id(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t second_distinct_event = build_test_event_for_distinct("second", "device-2", "{}");
    storage.events[1].data = second_distinct_event.data;
    storage.events[1].size = second_distinct_event.length;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 2u);
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
    assert(!storage.events[1].pending);
    assert(storage.events[1].consumed);
    assert(!storage.events[1].dead_lettered);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "second", 6u));
    assert(!bytes_contains(transport.last_chunk, transport.last_chunk_size, "first", 5u));
}

static void test_v2_flush_dead_letters_semantically_invalid_event(void)
{
    static const uint8_t invalid_event[] = {0xa0u};

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)invalid_event;
    storage.events[0].size = sizeof(invalid_event);
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(storage.events[0].dead_lettered);
    assert(!storage.events[0].consumed);
    assert(storage.events[1].consumed);
    assert(!storage.events[1].dead_lettered);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "second", 6u));
    assert(!bytes_contains(transport.last_chunk, transport.last_chunk_size, "first", 5u));
}

static void test_2xx_consumes_events(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(storage.events[0].consumed);
    assert(storage.events[1].consumed);
    assert(!storage.events[0].dead_lettered);
    assert(!storage.events[1].dead_lettered);
}

static void assert_rejected_dead_letters(honch_transport_result_t result)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = result, .status = HONCH_ERROR_REJECTED};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(!storage.events[0].pending);
    assert(!storage.events[0].consumed);
    assert(storage.events[0].dead_lettered);
}

static void test_401_dead_letters_events(void)
{
    assert_rejected_dead_letters(HONCH_TRANSPORT_AUTH_ERROR);
}

static void test_400_dead_letters_events(void)
{
    assert_rejected_dead_letters(HONCH_TRANSPORT_REJECTED);
}

static void assert_retry_preserves_events(honch_status_t status)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_RETRY, .status = status};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);

    assert(honch_queue_flush_locked(&client) == status);
    assert(storage.events[0].pending);
    assert(!storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_429_preserves_events(void)
{
    assert_retry_preserves_events(HONCH_ERROR_RATE_LIMITED);
}

static void test_500_preserves_events(void)
{
    assert_retry_preserves_events(HONCH_ERROR_SERVER);
}

static void test_network_error_preserves_events(void)
{
    assert_retry_preserves_events(HONCH_ERROR_TRANSPORT);
}

int main(void)
{
    test_hqr1_validate_rejects_embedded_nul_in_required_strings();
    test_hqr1_rejects_values_deeper_than_wire_v2_limit();
    test_hqr1_rejects_duplicate_top_level_property_keys();
    test_hqr1_rejects_duplicate_nested_map_keys();
    test_2xx_consumes_events();
    test_v2_chunk_transport_consumes_events_on_acceptance();
    test_v2_chunk_transport_splits_oversized_batch_without_dead_lettering_first_event();
    test_v2_batch_shrink_does_not_retry_linearly();
    test_v2_chunk_transport_preserves_string_properties();
    test_v2_flush_borrows_string_property_values();
    test_v2_flush_accepts_queued_empty_string_property_value();
    test_v2_chunk_transport_preserves_nested_properties();
    test_v2_chunk_transport_preserves_float64_properties();
    test_v2_chunk_transport_preserves_float32_bool_and_null_properties();
    test_v2_chunk_transport_preserves_half_float_as_float32_property();
    test_v2_flush_rejects_non_record_payload();
    test_v2_chunk_transport_preserves_device_boot_event();
    test_v2_final_chunk_stored_response_preserves_event();
    test_v2_multi_frame_flush_passes_stream_id_to_transport();
    test_flush_uses_storage_batch_read_when_available();
    test_flush_rejects_overreported_storage_batch_count();
    test_v2_flush_splits_batches_by_distinct_id();
    test_v2_flush_dead_letters_semantically_invalid_event();
    test_401_dead_letters_events();
    test_400_dead_letters_events();
    test_429_preserves_events();
    test_500_preserves_events();
    test_network_error_preserves_events();
    return 0;
}
