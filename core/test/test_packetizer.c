#include "honch/core/packetizer.h"
#include "honch_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_PACKET_HEADER_SIZE 20u
#define HONCH_TEST_SEQUENCE 7u

typedef struct fake_storage {
    const uint8_t *message;
    size_t message_size;
    uint64_t sequence;
    bool has_message;
    bool consumed;
    bool dead_lettered;
    honch_status_t consume_status;
    honch_client_t *client;
    bool require_client_lock_on_peek;
    bool peek_saw_client_lock;
    size_t read_calls;
} fake_storage_t;

typedef struct fake_clock {
    uint64_t now_ms;
    uint64_t uptime_ms;
} fake_clock_t;

static uint64_t fake_clock_now_ms(void *ctx)
{
    return ((const fake_clock_t *)ctx)->now_ms;
}

static uint64_t fake_clock_uptime_ms(void *ctx)
{
    return ((const fake_clock_t *)ctx)->uptime_ms;
}

static honch_status_t fake_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    if (storage == NULL || buffer == NULL || offset > storage->message_size ||
        buffer_size > storage->message_size - offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    storage->read_calls++;
    memcpy(buffer, storage->message + offset, buffer_size);
    return HONCH_OK;
}

static honch_status_t fake_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    if (storage == NULL || reader == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (!storage->has_message) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }
    if (storage->require_client_lock_on_peek && storage->client != NULL) {
        storage->peek_saw_client_lock = true;
    }

    *reader = (honch_storage_reader_t) {
        .ctx = storage,
        .read = fake_reader_read,
        .total_size = storage->message_size,
        .sequence = storage->sequence
    };
    return HONCH_OK;
}

static honch_status_t fake_queue_consume(void *ctx, uint64_t sequence)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    if (storage == NULL || sequence != storage->sequence) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (storage->consume_status != HONCH_OK) {
        return storage->consume_status;
    }

    storage->consumed = true;
    storage->has_message = false;
    return HONCH_OK;
}

static honch_status_t fake_queue_dead_letter(void *ctx, uint64_t sequence)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    if (storage == NULL || sequence != storage->sequence) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    storage->dead_lettered = true;
    return HONCH_OK;
}

static void fake_client_with_storage(honch_client_t *client, fake_storage_t *storage, honch_event_queue_ops_t *ops)
{
    *ops = (honch_event_queue_ops_t) {
        .queue_peek = fake_queue_peek,
        .queue_consume = fake_queue_consume,
        .queue_dead_letter = fake_queue_dead_letter,
        .ctx = storage
    };

    *client = (honch_client_t){0};
    client->event_queue = ops;
    client->device_id = "device-1";
    client->device_model = "model-x";
    client->firmware_version = "1.0.0";
    client->sdk_platform = "posix";
    client->environment = "production";
    client->max_event_bytes = HONCH_DEFAULT_MAX_EVENT_BYTES;
    storage->client = client;
}

static void fake_client_destroy(honch_client_t *client)
{
    (void)client;
}

static uint64_t read_u64_be(const uint8_t *buffer)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; i++) {
        value = (value << 8u) | buffer[i];
    }
    return value;
}

static uint32_t read_u32_be(const uint8_t *buffer)
{
    uint32_t value = 0u;
    for (size_t i = 0u; i < 4u; i++) {
        value = (value << 8u) | buffer[i];
    }
    return value;
}

static uint16_t read_u16_be(const uint8_t *buffer)
{
    return (uint16_t)(((uint16_t)buffer[0] << 8u) | buffer[1]);
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

static uint64_t packetizer_base_time_ms(const uint8_t *buffer, size_t out_size)
{
    assert(out_size > HONCH_PACKET_HEADER_SIZE);
    const uint8_t *message = buffer + HONCH_PACKET_HEADER_SIZE;
    size_t message_size = out_size - HONCH_PACKET_HEADER_SIZE;
    size_t offset = 0u;
    uint64_t value = 0u;
    assert(read_test_uvarint(message, message_size, &offset, &value));
    assert(value == 2u);
    assert(read_test_uvarint(message, message_size, &offset, &value));
    assert(value == 1u);
    assert(read_test_uvarint(message, message_size, &offset, &value));
    assert(read_test_uvarint(message, message_size, &offset, &value));
    return value;
}

static honch_payload_t build_test_record_at(uint64_t timestamp_ms)
{
    honch_payload_t payload = {0};
    assert(honch_event_record_build(
        "boot",
        "user-1",
        NULL,
        timestamp_ms,
        NULL,
        0u,
        &payload) == HONCH_OK);
    return payload;
}

static honch_payload_t build_test_record(void)
{
    return build_test_record_at(1700000000000ULL);
}

static void test_tiny_buffer_rejected(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);

    uint8_t buffer[HONCH_PACKET_HEADER_SIZE - 1u] = {0};
    size_t out_size = 0u;
    bool complete = false;
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(!storage.consumed);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_begin_rejects_queued_record_larger_than_max_event_bytes(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    client.max_event_bytes = message.length - 1u;
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(!packetizer.active);
    assert(!storage.consumed);

    fake_client_destroy(&client);
    free(message.data);
}

static void test_single_chunk_message_has_first_and_final_flags(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[256] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);

    assert(out_size > HONCH_PACKET_HEADER_SIZE);
    assert(complete);
    assert(buffer[0] == 1u);
    assert(buffer[1] == 1u);
    assert(buffer[2] == 0x03u);
    assert(buffer[3] == 0u);
    assert(read_u64_be(buffer + 4u) == HONCH_TEST_SEQUENCE);
    assert(read_u32_be(buffer + 12u) == 0u);
    assert(read_u16_be(buffer + 16u) == out_size - HONCH_PACKET_HEADER_SIZE);
    assert(read_u16_be(buffer + 18u) != 0u);
    assert(buffer[HONCH_PACKET_HEADER_SIZE] == 0x02u);
    assert(memcmp(buffer + HONCH_PACKET_HEADER_SIZE, "HQR1", 4u) != 0);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_multi_chunk_message_offsets_increase(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t first[HONCH_PACKET_HEADER_SIZE + 2u] = {0};
    uint8_t second[HONCH_PACKET_HEADER_SIZE + 2u] = {0};
    size_t first_size = 0u;
    size_t second_size = 0u;
    bool complete = true;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, first, sizeof(first), &first_size, &complete) == HONCH_OK);
    assert(!complete);
    assert(first[2] == 0x01u);
    assert(read_u32_be(first + 12u) == 0u);
    assert(read_u16_be(first + 16u) == 2u);

    assert(honch_packetizer_next(&packetizer, second, sizeof(second), &second_size, &complete) == HONCH_OK);
    assert(!complete);
    assert(second[2] == 0x00u);
    assert(read_u32_be(second + 12u) == 2u);
    assert(read_u16_be(second + 16u) == 2u);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_abort_does_not_consume_storage(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    assert(!storage.consumed);
    assert(storage.has_message);
    assert(!packetizer.active);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_confirm_consumes_storage(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[256] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);
    assert(complete);
    assert(honch_packetizer_confirm(&packetizer) == HONCH_OK);
    assert(storage.consumed);
    assert(!storage.has_message);
    assert(!packetizer.active);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_confirm_failure_invalidates_packetizer_after_releasing_client(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true,
        .consume_status = HONCH_ERROR_IO
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[256] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);
    assert(complete);
    assert(honch_packetizer_confirm(&packetizer) == HONCH_ERROR_IO);
    assert(!packetizer.active);
    assert(packetizer.client == NULL);
    assert(honch_packetizer_abort(&packetizer) == HONCH_ERROR_INVALID_ARGUMENT);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_packetizer_peek_runs_under_client_lock(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true,
        .require_client_lock_on_peek = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(storage.peek_saw_client_lock);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_packetizer_reconstructs_boot_relative_timestamps_when_wall_clock_is_valid(void)
{
    static const uint64_t queued_uptime_ms = 12000u;
    static const uint64_t flush_wall_time_ms = 1700000600000ULL;
    static const uint64_t flush_uptime_ms = 600000u;
    static const uint64_t expected_event_time_ms = 1700000012000ULL;

    honch_payload_t message = build_test_record_at(queued_uptime_ms);
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    fake_clock_t clock = {
        .now_ms = flush_wall_time_ms,
        .uptime_ms = flush_uptime_ms
    };
    honch_platform_ops_t platform = {
        .now_ms = fake_clock_now_ms,
        .uptime_ms = fake_clock_uptime_ms,
        .ctx = &clock
    };
    client.platform = &platform;
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[256] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);
    assert(complete);
    assert(packetizer_base_time_ms(buffer, out_size) == expected_event_time_ms);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_packetizer_encodes_once_regardless_of_chunk_count(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t chunk[HONCH_PACKET_HEADER_SIZE + 2u] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    while (!complete) {
        assert(honch_packetizer_next(&packetizer, chunk, sizeof(chunk), &out_size, &complete) == HONCH_OK);
    }
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    /* The queued event is read+encoded exactly once (in _begin), never per-chunk. */
    assert(storage.read_calls == 1u);
    fake_client_destroy(&client);
    free(message.data);
}

static void test_multi_chunk_reassembles_to_full_message(void)
{
    honch_payload_t message = build_test_record();
    fake_storage_t storage = {
        .message = message.data,
        .message_size = message.length,
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_event_queue_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    /* Reference: a single big-buffer chunk carries the whole encoded message. */
    uint8_t big[512] = {0};
    size_t big_size = 0u;
    bool complete = false;
    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, big, sizeof(big), &big_size, &complete) == HONCH_OK);
    assert(complete);
    size_t full_len = big_size - HONCH_PACKET_HEADER_SIZE;
    uint8_t full[512] = {0};
    memcpy(full, big + HONCH_PACKET_HEADER_SIZE, full_len);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);

    /* Tiny chunks: reassembled payloads must equal the full message byte-for-byte. */
    storage.has_message = true;
    uint8_t small[HONCH_PACKET_HEADER_SIZE + 3u] = {0};
    uint8_t reassembled[512] = {0};
    size_t r = 0u;
    complete = false;
    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    while (!complete) {
        size_t sz = 0u;
        assert(honch_packetizer_next(&packetizer, small, sizeof(small), &sz, &complete) == HONCH_OK);
        size_t payload = sz - HONCH_PACKET_HEADER_SIZE;
        memcpy(reassembled + r, small + HONCH_PACKET_HEADER_SIZE, payload);
        r += payload;
    }
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    assert(r == full_len);
    assert(memcmp(reassembled, full, full_len) == 0);
    fake_client_destroy(&client);
    free(message.data);
}

int main(void)
{
    test_tiny_buffer_rejected();
    test_begin_rejects_queued_record_larger_than_max_event_bytes();
    test_single_chunk_message_has_first_and_final_flags();
    test_multi_chunk_message_offsets_increase();
    test_abort_does_not_consume_storage();
    test_confirm_consumes_storage();
    test_confirm_failure_invalidates_packetizer_after_releasing_client();
    test_packetizer_peek_runs_under_client_lock();
    test_packetizer_reconstructs_boot_relative_timestamps_when_wall_clock_is_valid();
    test_packetizer_encodes_once_regardless_of_chunk_count();
    test_multi_chunk_reassembles_to_full_message();
    return 0;
}
