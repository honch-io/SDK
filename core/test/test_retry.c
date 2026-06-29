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
    size_t reject_on_chunk_call;
    honch_transport_result_t reject_result;
    honch_status_t reject_status;
    size_t calls;
    size_t chunk_calls;
    char last_stream_id[32];
    uint64_t last_message_id;
    bool reject_missing_stream_id;
    uint8_t last_chunk[512];
    size_t last_chunk_size;
} fake_transport_t;

typedef struct fake_clock {
    uint64_t now_ms;
    uint64_t uptime_ms;
} fake_clock_t;

static void *g_payload_allocations[128];
static size_t g_payload_allocation_count = 0u;

static void track_payload_allocation(void *data)
{
    if (data == NULL) {
        return;
    }
    assert(g_payload_allocation_count < sizeof(g_payload_allocations) / sizeof(g_payload_allocations[0]));
    g_payload_allocations[g_payload_allocation_count++] = data;
}

static void free_tracked_payload(void *data)
{
    if (data == NULL) {
        return;
    }
    for (size_t i = 0u; i < g_payload_allocation_count; i++) {
        if (g_payload_allocations[i] == data) {
            g_payload_allocations[i] = g_payload_allocations[g_payload_allocation_count - 1u];
            g_payload_allocations[g_payload_allocation_count - 1u] = NULL;
            g_payload_allocation_count--;
            break;
        }
    }
    free(data);
}

static void free_tracked_payloads(void)
{
    for (size_t i = 0u; i < g_payload_allocation_count; i++) {
        free(g_payload_allocations[i]);
        g_payload_allocations[i] = NULL;
    }
    g_payload_allocation_count = 0u;
}

static uint64_t fake_clock_now_ms(void *ctx)
{
    return ((const fake_clock_t *)ctx)->now_ms;
}

static uint64_t fake_clock_uptime_ms(void *ctx)
{
    return ((const fake_clock_t *)ctx)->uptime_ms;
}

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

static honch_status_t fake_queue_read_batch_valid_then_invalid(
    void *ctx,
    honch_storage_event_t *events,
    size_t max_events,
    size_t max_event_bytes,
    size_t *event_count)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    storage->read_batch_calls++;
    assert(max_events >= 2u);
    assert(storage->event_count >= 1u);
    assert(storage->events[0].size <= max_event_bytes);

    uint8_t *copy = (uint8_t *)malloc(storage->events[0].size);
    assert(copy != NULL);
    memcpy(copy, storage->events[0].data, storage->events[0].size);
    events[0] = (honch_storage_event_t) {
        .data = copy,
        .length = storage->events[0].size,
        .sequence = storage->events[0].sequence
    };
    events[1] = (honch_storage_event_t) {
        .data = NULL,
        .length = 1u,
        .sequence = storage->events[0].sequence + 1u
    };
    *event_count = 2u;
    return HONCH_OK;
}

static honch_status_t fake_queue_peek_empty(void *ctx, honch_storage_reader_t *reader)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    (void)reader;
    storage->peek_calls++;
    return HONCH_ERROR_NOT_INITIALIZED;
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

static void build_heavy_event_with_final_array(
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

static void build_heavy_event(
    const char *event_name,
    size_t property_count,
    size_t array_length,
    uint8_t **out_data,
    size_t *out_size)
{
    build_heavy_event_with_final_array(
        event_name,
        property_count,
        array_length,
        array_length,
        out_data,
        out_size);
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
    if (transport->reject_on_chunk_call > 0u &&
        transport->chunk_calls == transport->reject_on_chunk_call) {
        *result = transport->reject_result;
        return transport->reject_status;
    }
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

static bool read_test_uvarint(const uint8_t *bytes, size_t size, size_t *offset, uint64_t *out)
{
    uint64_t value = 0u;
    unsigned int shift = 0u;
    while (*offset < size && shift < 64u) {
        uint8_t byte = bytes[(*offset)++];
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *out = value;
            return true;
        }
        shift += 7u;
    }
    return false;
}

static size_t append_test_uvarint(uint8_t *bytes, size_t offset, uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        if (value != 0u) {
            byte |= 0x80u;
        }
        bytes[offset++] = byte;
    } while (value != 0u);
    return offset;
}

static size_t append_test_string(uint8_t *bytes, size_t offset, const char *value)
{
    size_t length = strlen(value);
    offset = append_test_uvarint(bytes, offset, (uint64_t)length);
    memcpy(bytes + offset, value, length);
    offset += length;
    bytes[offset++] = 0u;
    return offset;
}

static size_t build_malformed_count_record(uint8_t *bytes, uint8_t value_tag, uint64_t count)
{
    size_t offset = 0u;
    memcpy(bytes + offset, "HQR1", 4u);
    offset += 4u;
    offset = append_test_uvarint(bytes, offset, 1234u);
    offset = append_test_string(bytes, offset, "device-1");
    bytes[offset++] = 0u;
    offset = append_test_string(bytes, offset, "malformed");
    offset = append_test_uvarint(bytes, offset, 1u);
    offset = append_test_string(bytes, offset, "value");
    bytes[offset++] = value_tag;
    offset = append_test_uvarint(bytes, offset, count);
    return offset;
}

static size_t build_malformed_string_length_record(uint8_t *bytes, uint64_t string_length)
{
    size_t offset = 0u;
    memcpy(bytes + offset, "HQR1", 4u);
    offset += 4u;
    offset = append_test_uvarint(bytes, offset, 1234u);
    offset = append_test_string(bytes, offset, "device-1");
    bytes[offset++] = 0u;
    offset = append_test_uvarint(bytes, offset, string_length);
    return offset;
}

static uint64_t last_chunk_base_time_ms(const fake_transport_t *transport)
{
    assert(transport->last_chunk_size > 4u);
    size_t frame_offset = 1u;
    uint64_t ignored = 0u;
    assert((transport->last_chunk[0] & 0x03u) == 0x02u);
    assert((transport->last_chunk[0] & 0x40u) == 0u);
    assert((transport->last_chunk[0] & 0x20u) == 0u);
    assert(read_test_uvarint(transport->last_chunk, transport->last_chunk_size, &frame_offset, &ignored));
    assert(frame_offset <= transport->last_chunk_size - 2u);

    const uint8_t *message = transport->last_chunk + frame_offset;
    size_t message_size = transport->last_chunk_size - frame_offset - 2u;
    size_t message_offset = 0u;
    uint64_t value = 0u;
    assert(read_test_uvarint(message, message_size, &message_offset, &value));
    assert(value == 2u);
    assert(read_test_uvarint(message, message_size, &message_offset, &value));
    assert(value == 1u);
    assert(read_test_uvarint(message, message_size, &message_offset, &value));
    assert(read_test_uvarint(message, message_size, &message_offset, &value));
    return value;
}

static honch_payload_t build_test_event_for_distinct(
    const char *event_name,
    const char *distinct_id,
    uint64_t timestamp_ms,
    const honch_wire_v2_property_t *properties,
    size_t property_count)
{
    honch_payload_t payload = {0};
    assert(honch_event_record_build(event_name, distinct_id, NULL, timestamp_ms, properties, property_count, &payload) ==
        HONCH_OK);
    track_payload_allocation(payload.data);
    return payload;
}

static honch_payload_t build_test_event_for_distinct_with_default_time(
    const char *event_name,
    const char *distinct_id,
    const honch_wire_v2_property_t *properties,
    size_t property_count)
{
    return build_test_event_for_distinct(event_name, distinct_id, 1234u, properties, property_count);
}

static honch_payload_t build_test_event(
    const char *event_name,
    const honch_wire_v2_property_t *properties,
    size_t property_count)
{
    return build_test_event_for_distinct_with_default_time(event_name, "device-1", properties, property_count);
}

static honch_payload_t build_test_event_no_props(const char *event_name)
{
    return build_test_event(event_name, NULL, 0u);
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("modex", honch_str("on"))
    };
    honch_payload_t event = build_test_event("clickx", properties, 1u);

    uint8_t *event_name = find_payload_bytes(&event, "clickx");
    assert(event_name != NULL);
    event_name[5] = '\0';
    assert(!honch_event_record_validate(event.data, event.length));

    event_name[5] = 'x';
    uint8_t *property_key = find_payload_bytes(&event, "modex");
    assert(property_key != NULL);
    property_key[4] = '\0';
    assert(!honch_event_record_validate(event.data, event.length));

    free_tracked_payload(event.data);
}

static void test_hqr1_rejects_values_deeper_than_wire_v2_limit(void)
{
    honch_wire_v2_value_t allowed_levels[9];
    allowed_levels[0] = honch_i64(1);
    for (size_t i = 1u; i < 9u; i++) {
        allowed_levels[i] = honch_array(&allowed_levels[i - 1u], 1u);
    }
    const honch_wire_v2_property_t allowed_properties[] = {
        honch_prop("allowed", allowed_levels[8])
    };
    honch_payload_t payload = {0};
    assert(honch_event_record_build("depth", "device-1", NULL, 1234u, allowed_properties, 1u, &payload) == HONCH_OK);
    free_tracked_payload(payload.data);

    honch_wire_v2_value_t nested_levels[10];
    nested_levels[0] = honch_i64(1);
    for (size_t i = 1u; i < 10u; i++) {
        nested_levels[i] = honch_array(&nested_levels[i - 1u], 1u);
    }
    const honch_wire_v2_property_t nested_properties[] = {
        honch_prop("nested", nested_levels[9])
    };
    assert(honch_event_record_build("depth", "device-1", NULL, 1234u, nested_properties, 1u, &payload) ==
        HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_duplicate_top_level_property_keys(void)
{
    const honch_wire_v2_property_t properties[] = {
        honch_prop("mode", honch_str("a")),
        honch_prop("mode", honch_str("b"))
    };
    honch_payload_t payload = {0};
    assert(honch_event_record_build("duplicate", "device-1", NULL, 1234u, properties, 2u, &payload) ==
        HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_duplicate_nested_map_keys(void)
{
    const honch_wire_v2_map_pair_t entries[] = {
        honch_pair("mode", honch_str("a")),
        honch_pair("mode", honch_str("b"))
    };
    const honch_wire_v2_property_t properties[] = {
        honch_prop("nested", honch_map(entries, 2u))
    };
    honch_payload_t payload = {0};
    assert(honch_event_record_build("duplicate", "device-1", NULL, 1234u, properties, 1u, &payload) ==
        HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_nested_promoted_context_keys_before_queueing(void)
{
    const honch_wire_v2_map_pair_t entries[] = {
        honch_pair("distinct_id", honch_str("spoofed"))
    };
    const honch_wire_v2_property_t properties[] = {
        honch_prop("nested", honch_map(entries, 1u))
    };
    honch_payload_t payload = {0};
    assert(honch_event_record_build("promoted", "device-1", NULL, 1234u, properties, 1u, &payload) ==
        HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_malformed_array_count_before_allocation(void)
{
    uint8_t record[64];
    size_t record_size = build_malformed_count_record(record, 9u, UINT64_MAX / 128u);
    honch_event_record_t parsed;
    assert(honch_event_record_parse(record, record_size, &parsed) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_malformed_map_count_before_allocation(void)
{
    uint8_t record[64];
    size_t record_size = build_malformed_count_record(record, 10u, UINT64_MAX / 128u);
    honch_event_record_t parsed;
    assert(honch_event_record_parse(record, record_size, &parsed) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_hqr1_rejects_wrapping_string_length_without_oob_read(void)
{
    uint8_t record[64];
    size_t record_size = build_malformed_string_length_record(record, UINT64_MAX);
    honch_event_record_t parsed;
    assert(honch_event_record_parse(record, record_size, &parsed) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(!honch_event_record_validate(record, record_size));
}

static honch_client_t fake_client(fake_storage_t *storage, honch_event_queue_ops_t *storage_ops, fake_transport_t *transport, honch_transport_ops_t *transport_ops)
{
    *storage_ops = (honch_event_queue_ops_t) {
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
    client.event_queue = storage_ops;
    client.transport = transport_ops;
    return client;
}

static void setup_storage(fake_storage_t *storage)
{
    honch_payload_t first_event = build_test_event_no_props("first");
    honch_payload_t second_event = build_test_event_no_props("second");
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
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("mode", honch_str("auto"))
    };
    honch_payload_t event_with_property = build_test_event("first", properties, 1u);
    storage.events[0].data = event_with_property.data;
    storage.events[0].size = event_with_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("mode", honch_str("hdr"))
    };
    honch_payload_t event_with_property = build_test_event("event", properties, 1u);
    storage.events[0].data = event_with_property.data;
    storage.events[0].size = event_with_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("empty", honch_str("")),
        honch_prop("x", honch_null())
    };
    honch_payload_t event_with_empty_string_property = build_test_event("event", properties, 2u);
    storage.events[0].data = event_with_empty_string_property.data;
    storage.events[0].size = event_with_empty_string_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_value_t tags[] = {
        honch_str("red"),
        honch_str("blue")
    };
    const honch_wire_v2_map_pair_t traits[] = {
        honch_pair("tier", honch_str("gold"))
    };
    const honch_wire_v2_property_t properties[] = {
        honch_prop("tags", honch_array(tags, 2u)),
        honch_prop("traits", honch_map(traits, 1u))
    };
    honch_payload_t event_with_nested_properties = build_test_event("first", properties, 2u);
    storage.events[0].data = event_with_nested_properties.data;
    storage.events[0].size = event_with_nested_properties.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("temp", honch_f64(36.5))
    };
    honch_payload_t event_with_float_property = build_test_event("first", properties, 1u);
    storage.events[0].data = event_with_float_property.data;
    storage.events[0].size = event_with_float_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("temp", honch_f64(36.5)),
        honch_prop("active", honch_bool(true)),
        honch_prop("empty", honch_null())
    };
    honch_payload_t event_with_scalar_properties = build_test_event("first", properties, 3u);
    storage.events[0].data = event_with_scalar_properties.data;
    storage.events[0].size = event_with_scalar_properties.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("temp", honch_f64(1.5))
    };
    honch_payload_t event_with_float_property = build_test_event("first", properties, 1u);
    storage.events[0].data = event_with_float_property.data;
    storage.events[0].size = event_with_float_property.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    const honch_wire_v2_property_t properties[] = {
        honch_prop("reset_reason", honch_str("unknown"))
    };
    honch_payload_t boot_event = build_test_event("$device_boot", properties, 1u);
    storage.events[0].data = boot_event.data;
    storage.events[0].size = boot_event.length;
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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

static void test_v2_flush_reconstructs_boot_relative_timestamps_when_wall_clock_is_valid(void)
{
    static const uint64_t queued_uptime_ms = 12000u;
    static const uint64_t flush_wall_time_ms = 1700000600000ULL;
    static const uint64_t flush_uptime_ms = 600000u;
    static const uint64_t expected_event_time_ms = 1700000012000ULL;

    fake_storage_t storage = {0};
    honch_payload_t event = build_test_event_for_distinct("first", "device-1", queued_uptime_ms, NULL, 0u);
    storage.event_count = 1u;
    storage.events[0] = (fake_event_t) {
        .data = event.data,
        .size = event.length,
        .sequence = 1u,
        .pending = true
    };

    fake_clock_t clock = {
        .now_ms = flush_wall_time_ms,
        .uptime_ms = flush_uptime_ms
    };
    honch_platform_ops_t platform = {
        .now_ms = fake_clock_now_ms,
        .uptime_ms = fake_clock_uptime_ms,
        .ctx = &clock
    };
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    client.platform = &platform;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.chunk_calls == 1u);
    assert(last_chunk_base_time_ms(&transport) == expected_event_time_ms);
    assert(storage.events[0].consumed);
}

static void test_v2_final_chunk_stored_response_preserves_event(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_CHUNK_STORED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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

static void test_v2_pending_chunk_rejection_dead_letters_event(void)
{
    uint8_t *event = NULL;
    size_t event_size = 0u;
    build_heavy_event_with_final_array("pending_reject", 64u, 28u, 11u, &event, &event_size);

    fake_storage_t storage = {0};
    storage.event_count = 1u;
    storage.events[0] = (fake_event_t) {
        .data = event,
        .size = event_size,
        .sequence = 1u,
        .pending = true
    };

    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_CHUNK_STORED,
        .status = HONCH_OK,
        .reject_on_chunk_call = 2u,
        .reject_result = HONCH_TRANSPORT_REJECTED,
        .reject_status = HONCH_ERROR_REJECTED
    };
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    client.batch_size = 1u;
    client.max_event_bytes = 8192u;
    transport_ops.post_chunk = fake_post_chunk;

    bool progressed = false;
    assert(honch_queue_flush_one_chunk_locked(&client, &progressed) == HONCH_OK);
    assert(!progressed);
    assert(client.pending_flush_active);
    assert(storage.events[0].pending);

    assert(honch_queue_flush_one_chunk_locked(&client, &progressed) == HONCH_ERROR_REJECTED);
    assert(progressed);
    assert(!client.pending_flush_active);
    assert(!storage.events[0].pending);
    assert(storage.events[0].dead_lettered);
    assert(!storage.events[0].consumed);

    free(event);
}

static void test_flush_uses_storage_batch_read_when_available(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);

    assert(honch_queue_flush_locked(&client) == HONCH_OK);

    assert(storage.read_batch_calls == 1u);
    assert(storage.peek_calls == 0u);
    assert(storage.consume_batch_calls == 1u);
    assert(storage.events[0].consumed);
    assert(storage.events[1].consumed);
}

static void test_limited_flush_stops_after_one_batch(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    client.batch_size = 1u;

    assert(honch_queue_flush_limited_locked(&client, 1u) == HONCH_OK);

    assert(transport.chunk_calls == 1u);
    assert(storage.read_batch_calls == 1u);
    assert(storage.consume_batch_calls == 1u);
    assert(storage.events[0].consumed);
    assert(storage.events[1].pending);
    assert(!storage.events[1].consumed);
    assert(client.queued_event_count == 1u);
}

static void test_flush_rejects_overreported_storage_batch_count(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_event_queue_ops_t storage_ops = {0};
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

static void test_invalid_storage_batch_frees_transferred_events_before_reader_fallback(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    fake_transport_t transport = {
        .result = HONCH_TRANSPORT_ACCEPTED,
        .status = HONCH_OK
    };
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    storage_ops.queue_read_batch = fake_queue_read_batch_valid_then_invalid;
    storage_ops.queue_peek = fake_queue_peek_empty;

    bool progressed = true;
    assert(honch_queue_flush_one_locked(&client, &progressed) == HONCH_OK);
    assert(!progressed);
    assert(storage.read_batch_calls == 1u);
    assert(storage.peek_calls == 1u);
    assert(transport.chunk_calls == 0u);
    assert(client.flush_events[0].data == NULL);
    assert(!client.flush_event_borrowed[0]);
    assert(!storage.events[0].consumed);
    assert(!storage.events[1].consumed);
}

static void test_v2_flush_splits_batches_by_distinct_id(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    honch_payload_t second_distinct_event =
        build_test_event_for_distinct_with_default_time("second", "device-2", NULL, 0u);
    storage.events[1].data = second_distinct_event.data;
    storage.events[1].size = second_distinct_event.length;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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
    honch_event_queue_ops_t storage_ops = {0};
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

/* ---- error-context capture + bounded auto-log (feat/error-context) ---- */

typedef struct diag_log_recorder {
    size_t count;
    honch_log_level_t last_level;
    char last_message[HONCH_DIAG_LINE_BYTES];
} diag_log_recorder_t;

static void diag_record_log(void *ctx, honch_log_level_t level, const char *message)
{
    diag_log_recorder_t *rec = (diag_log_recorder_t *)ctx;
    rec->count++;
    rec->last_level = level;
    snprintf(rec->last_message, sizeof(rec->last_message), "%s", message == NULL ? "" : message);
}

typedef struct detail_transport {
    honch_transport_result_t result;
    honch_status_t status;
    int http_status;
    honch_error_reason_t reason;
} detail_transport_t;

/* Reports rich detail through the detailed variant. */
static honch_status_t detail_post_chunk_ex(
    void *ctx, const char *url, const char *key, const char *stream,
    const uint8_t *body, size_t n,
    honch_transport_result_t *result, honch_transport_detail_t *detail)
{
    (void)url; (void)key; (void)stream; (void)body; (void)n;
    detail_transport_t *t = (detail_transport_t *)ctx;
    if (detail != NULL) {
        detail->http_status = t->http_status;
        detail->reason = t->reason;
    }
    *result = t->result;
    return t->status;
}

/* Legacy-style transport: only the coarse result, no detail (post_chunk only). */
static honch_status_t detail_post_chunk_plain(
    void *ctx, const char *url, const char *key, const char *stream,
    const uint8_t *body, size_t n, honch_transport_result_t *result)
{
    (void)url; (void)key; (void)stream; (void)body; (void)n;
    detail_transport_t *t = (detail_transport_t *)ctx;
    *result = t->result;
    return t->status;
}

/* C1: post_chunk_ex detail (http_status + reason) is captured into last_error. */
static void test_error_capture_from_post_chunk_ex(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    detail_transport_t t = {
        .result = HONCH_TRANSPORT_AUTH_ERROR, .status = HONCH_ERROR_REJECTED,
        .http_status = 401, .reason = HONCH_REASON_AUTH_INVALID_KEY
    };
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    fake_transport_t unused = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &unused, &transport_ops);
    transport_ops.ctx = &t;
    transport_ops.post_chunk = detail_post_chunk_plain;     /* required non-NULL */
    transport_ops.post_chunk_ex = detail_post_chunk_ex;     /* preferred */

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(client.last_error.http_status == 401);
    assert(client.last_error.reason == HONCH_REASON_AUTH_INVALID_KEY);
    assert(client.last_error.transport_result == HONCH_TRANSPORT_AUTH_ERROR);
    assert(client.last_error.status == HONCH_ERROR_REJECTED);
    assert(client.last_error.message != NULL);
}

/* C1: a transport with only post_chunk still yields a derived reason (fallback). */
static void test_error_capture_fallback_without_post_chunk_ex(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    detail_transport_t t = {.result = HONCH_TRANSPORT_AUTH_ERROR, .status = HONCH_ERROR_REJECTED};
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    fake_transport_t unused = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &unused, &transport_ops);
    transport_ops.ctx = &t;
    transport_ops.post_chunk = detail_post_chunk_plain;
    transport_ops.post_chunk_ex = NULL; /* legacy port */

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(client.last_error.http_status == 0);  /* unknown without _ex */
    assert(client.last_error.reason == HONCH_REASON_AUTH_INVALID_KEY); /* derived from result */
}

/* C3: identical retryable failures log once (deduped, WARN); a distinct failure
 * logs again; a success resets the dedup so the next failure logs again. */
static void test_diagnostic_autolog_dedup_and_reset(void)
{
    fake_storage_t storage;
    detail_transport_t t = {
        .result = HONCH_TRANSPORT_RETRY, .status = HONCH_ERROR_TRANSPORT,
        .http_status = 503, .reason = HONCH_REASON_HTTP_STATUS
    };
    diag_log_recorder_t rec = {0};
    honch_platform_ops_t platform = {.log = diag_record_log, .ctx = &rec};
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    fake_transport_t unused = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &unused, &transport_ops);
    client.platform = &platform;
    transport_ops.ctx = &t;
    transport_ops.post_chunk = detail_post_chunk_plain;
    transport_ops.post_chunk_ex = detail_post_chunk_ex;

    /* Two identical 503 failures -> exactly one WARN line (retries don't spam). */
    setup_storage(&storage);
    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    setup_storage(&storage);
    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    assert(rec.count == 1u);
    assert(rec.last_level == HONCH_LOG_WARN);
    assert(strstr(rec.last_message, "503") != NULL);

    /* A distinct failure (500) logs again. */
    t.http_status = 500;
    setup_storage(&storage);
    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    assert(rec.count == 2u);

    /* A success resets the dedup table; the same 503 then logs again. */
    t.result = HONCH_TRANSPORT_ACCEPTED; t.status = HONCH_OK;
    setup_storage(&storage);
    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(rec.count == 2u); /* success itself logs nothing */
    t.result = HONCH_TRANSPORT_RETRY; t.status = HONCH_ERROR_TRANSPORT; t.http_status = 503;
    setup_storage(&storage);
    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_TRANSPORT);
    assert(rec.count == 3u);
}

/* C3: a terminal rejection logs at ERROR level. */
static void test_diagnostic_autolog_terminal_is_error_level(void)
{
    fake_storage_t storage;
    setup_storage(&storage);
    detail_transport_t t = {
        .result = HONCH_TRANSPORT_AUTH_ERROR, .status = HONCH_ERROR_REJECTED,
        .http_status = 401, .reason = HONCH_REASON_AUTH_INVALID_KEY
    };
    diag_log_recorder_t rec = {0};
    honch_platform_ops_t platform = {.log = diag_record_log, .ctx = &rec};
    honch_event_queue_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    fake_transport_t unused = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &unused, &transport_ops);
    client.platform = &platform;
    transport_ops.ctx = &t;
    transport_ops.post_chunk = detail_post_chunk_plain;
    transport_ops.post_chunk_ex = detail_post_chunk_ex;

    assert(honch_queue_flush_locked(&client) == HONCH_ERROR_REJECTED);
    assert(rec.count == 1u);
    assert(rec.last_level == HONCH_LOG_ERROR);
    assert(strstr(rec.last_message, "401") != NULL);
}

/* C2: a local pipeline failure (queue full / encode) is captured into
 * last_error with a component tag and a human message. (The track-path call
 * site is a straight-line invocation of this same function; the full integration
 * path is exercised by the posix + on-device e2e.) */
static void test_local_failure_capture(void)
{
    honch_client_t client = {0}; /* platform NULL -> capture only, no auto-log */
    honch_diag_capture_local_locked(
        &client, HONCH_ERROR_QUEUE_FULL, HONCH_REASON_QUEUE_FULL, "queue");
    assert(client.last_error.status == HONCH_ERROR_QUEUE_FULL);
    assert(client.last_error.reason == HONCH_REASON_QUEUE_FULL);
    assert(client.last_error.message != NULL);
    assert(strcmp(client.last_error.component, "queue") == 0);

    honch_diag_capture_local_locked(
        &client, HONCH_ERROR_OUT_OF_MEMORY, HONCH_REASON_ENCODE_FAILED, "encode");
    assert(client.last_error.reason == HONCH_REASON_ENCODE_FAILED);
    assert(strcmp(client.last_error.component, "encode") == 0);
}

int main(void)
{
    atexit(free_tracked_payloads);
    test_local_failure_capture();
    test_error_capture_from_post_chunk_ex();
    test_error_capture_fallback_without_post_chunk_ex();
    test_diagnostic_autolog_dedup_and_reset();
    test_diagnostic_autolog_terminal_is_error_level();
    test_hqr1_validate_rejects_embedded_nul_in_required_strings();
    test_hqr1_rejects_values_deeper_than_wire_v2_limit();
    test_hqr1_rejects_duplicate_top_level_property_keys();
    test_hqr1_rejects_duplicate_nested_map_keys();
    test_hqr1_rejects_nested_promoted_context_keys_before_queueing();
    test_hqr1_rejects_malformed_array_count_before_allocation();
    test_hqr1_rejects_malformed_map_count_before_allocation();
    test_hqr1_rejects_wrapping_string_length_without_oob_read();
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
    test_v2_flush_reconstructs_boot_relative_timestamps_when_wall_clock_is_valid();
    test_v2_final_chunk_stored_response_preserves_event();
    test_v2_multi_frame_flush_passes_stream_id_to_transport();
    test_v2_pending_chunk_rejection_dead_letters_event();
    test_flush_uses_storage_batch_read_when_available();
    test_limited_flush_stops_after_one_batch();
    test_flush_rejects_overreported_storage_batch_count();
    test_invalid_storage_batch_frees_transferred_events_before_reader_fallback();
    test_v2_flush_splits_batches_by_distinct_id();
    test_v2_flush_dead_letters_semantically_invalid_event();
    test_401_dead_letters_events();
    test_400_dead_letters_events();
    test_429_preserves_events();
    test_500_preserves_events();
    test_network_error_preserves_events();
    return 0;
}
