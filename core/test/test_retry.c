#include "honch_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_event {
    uint8_t *data;
    size_t size;
    uint64_t sequence;
    bool pending;
    bool consumed;
    bool dead_lettered;
} fake_event_t;

typedef struct fake_storage {
    fake_event_t events[2];
    size_t event_count;
    size_t peek_index;
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

static honch_client_t fake_client(fake_storage_t *storage, honch_storage_ops_t *storage_ops, fake_transport_t *transport, honch_transport_ops_t *transport_ops)
{
    *storage_ops = (honch_storage_ops_t) {
        .queue_peek = fake_queue_peek,
        .queue_consume = fake_queue_consume,
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
    static const uint8_t first_event[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa0u
    };
    static const uint8_t second_event[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x66u, 's', 'e', 'c', 'o', 'n', 'd',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa0u
    };

    memset(storage, 0, sizeof(*storage));
    storage->event_count = 2u;
    storage->events[0] = (fake_event_t) {
        .data = (uint8_t *)first_event,
        .size = sizeof(first_event),
        .sequence = 1u,
        .pending = true
    };
    storage->events[1] = (fake_event_t) {
        .data = (uint8_t *)second_event,
        .size = sizeof(second_event),
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

static void test_v2_chunk_transport_preserves_string_properties(void)
{
    static const uint8_t event_with_property[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa1u,
        0x64u, 'm', 'o', 'd', 'e',
        0x64u, 'a', 'u', 't', 'o'
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_property;
    storage.events[0].size = sizeof(event_with_property);
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

static void test_v2_chunk_transport_preserves_nested_properties(void)
{
    static const uint8_t event_with_nested_properties[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa2u,
        0x64u, 't', 'a', 'g', 's',
        0x82u,
        0x63u, 'r', 'e', 'd',
        0x64u, 'b', 'l', 'u', 'e',
        0x66u, 't', 'r', 'a', 'i', 't', 's',
        0xa1u,
        0x64u, 't', 'i', 'e', 'r',
        0x64u, 'g', 'o', 'l', 'd'
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_nested_properties;
    storage.events[0].size = sizeof(event_with_nested_properties);
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
    static const uint8_t event_with_float_property[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa1u,
        0x64u, 't', 'e', 'm', 'p',
        0xfbu, 0x40u, 0x42u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };
    static const uint8_t expected_float64_value[] = {
        0x06u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x42u, 0x40u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_float_property;
    storage.events[0].size = sizeof(event_with_float_property);
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
    static const uint8_t event_with_scalar_properties[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa3u,
        0x64u, 't', 'e', 'm', 'p',
        0xfau, 0x42u, 0x12u, 0x00u, 0x00u,
        0x66u, 'a', 'c', 't', 'i', 'v', 'e',
        0xf5u,
        0x65u, 'e', 'm', 'p', 't', 'y',
        0xf6u
    };
    static const uint8_t expected_float32_value[] = {
        0x05u, 0x00u, 0x00u, 0x12u, 0x42u
    };
    static const uint8_t expected_true_value[] = {
        0x11u, 0x02u
    };
    static const uint8_t expected_null_value[] = {
        0x13u, 0x00u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_scalar_properties;
    storage.events[0].size = sizeof(event_with_scalar_properties);
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
        (const char *)expected_float32_value,
        sizeof(expected_float32_value)));
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
    static const uint8_t event_with_half_float_property[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa1u,
        0x64u, 't', 'e', 'm', 'p',
        0xf9u, 0x3eu, 0x00u
    };
    static const uint8_t expected_float32_value[] = {
        0x05u, 0x00u, 0x00u, 0xc0u, 0x3fu
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_half_float_property;
    storage.events[0].size = sizeof(event_with_half_float_property);
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
        (const char *)expected_float32_value,
        sizeof(expected_float32_value)));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_bytes_properties(void)
{
    static const uint8_t event_with_bytes_property[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa1u,
        0x64u, 'b', 'l', 'o', 'b',
        0x43u, 0x01u, 0x02u, 0x03u
    };
    static const uint8_t expected_bytes_value[] = {
        0x08u, 0x03u, 0x01u, 0x02u, 0x03u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)event_with_bytes_property;
    storage.events[0].size = sizeof(event_with_bytes_property);
    storage.events[1].pending = false;
    fake_transport_t transport = {.result = HONCH_TRANSPORT_ACCEPTED, .status = HONCH_OK};
    honch_storage_ops_t storage_ops = {0};
    honch_transport_ops_t transport_ops = {0};
    honch_client_t client = fake_client(&storage, &storage_ops, &transport, &transport_ops);
    transport_ops.post_chunk = fake_post_chunk;

    assert(honch_queue_flush_locked(&client) == HONCH_OK);
    assert(transport.calls == 0u);
    assert(transport.chunk_calls == 1u);
    assert(bytes_contains(transport.last_chunk, transport.last_chunk_size, "blob", 4u));
    assert(bytes_contains(
        transport.last_chunk,
        transport.last_chunk_size,
        (const char *)expected_bytes_value,
        sizeof(expected_bytes_value)));
    assert(storage.events[0].consumed);
    assert(!storage.events[0].dead_lettered);
}

static void test_v2_chunk_transport_preserves_device_boot_event(void)
{
    static const uint8_t boot_event[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x6cu, '$', 'd', 'e', 'v', 'i', 'c', 'e', '_', 'b', 'o', 'o', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa1u,
        0x6cu, 'r', 'e', 's', 'e', 't', '_', 'r', 'e', 'a', 's', 'o', 'n',
        0x67u, 'u', 'n', 'k', 'n', 'o', 'w', 'n'
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)boot_event;
    storage.events[0].size = sizeof(boot_event);
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

static void test_v2_flush_splits_batches_by_distinct_id(void)
{
    static const uint8_t second_distinct_event[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x66u, 's', 'e', 'c', 'o', 'n', 'd',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '2',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa0u
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[1].data = (uint8_t *)second_distinct_event;
    storage.events[1].size = sizeof(second_distinct_event);
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
    static const uint8_t duplicate_property_event[] = {
        0xa4u,
        0x65u, 'e', 'v', 'e', 'n', 't',
        0x65u, 'f', 'i', 'r', 's', 't',
        0x6bu, 'd', 'i', 's', 't', 'i', 'n', 'c', 't', '_', 'i', 'd',
        0x68u, 'd', 'e', 'v', 'i', 'c', 'e', '-', '1',
        0x69u, 't', 'i', 'm', 'e', 's', 't', 'a', 'm', 'p',
        0x19u, 0x04u, 0xd2u,
        0x6au, 'p', 'r', 'o', 'p', 'e', 'r', 't', 'i', 'e', 's',
        0xa2u,
        0x64u, 'm', 'o', 'd', 'e',
        0x63u, 'h', 'd', 'r',
        0x64u, 'm', 'o', 'd', 'e',
        0x64u, 'a', 'u', 't', 'o'
    };

    fake_storage_t storage;
    setup_storage(&storage);
    storage.events[0].data = (uint8_t *)duplicate_property_event;
    storage.events[0].size = sizeof(duplicate_property_event);
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
    test_2xx_consumes_events();
    test_v2_chunk_transport_consumes_events_on_acceptance();
    test_v2_chunk_transport_preserves_string_properties();
    test_v2_chunk_transport_preserves_nested_properties();
    test_v2_chunk_transport_preserves_float64_properties();
    test_v2_chunk_transport_preserves_float32_bool_and_null_properties();
    test_v2_chunk_transport_preserves_half_float_as_float32_property();
    test_v2_chunk_transport_preserves_bytes_properties();
    test_v2_chunk_transport_preserves_device_boot_event();
    test_v2_final_chunk_stored_response_preserves_event();
    test_v2_multi_frame_flush_passes_stream_id_to_transport();
    test_v2_flush_splits_batches_by_distinct_id();
    test_v2_flush_dead_letters_semantically_invalid_event();
    test_401_dead_letters_events();
    test_400_dead_letters_events();
    test_429_preserves_events();
    test_500_preserves_events();
    test_network_error_preserves_events();
    return 0;
}
