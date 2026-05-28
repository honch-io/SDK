#include "honch/core/packetizer.h"
#include "honch_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
} fake_storage_t;

static honch_status_t fake_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    fake_storage_t *storage = (fake_storage_t *)ctx;
    if (storage == NULL || buffer == NULL || offset > storage->message_size ||
        buffer_size > storage->message_size - offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

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
        int lock_status = pthread_mutex_trylock(&storage->client->mutex);
        assert(lock_status == EBUSY);
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

static void fake_client_with_storage(honch_client_t *client, fake_storage_t *storage, honch_storage_ops_t *ops)
{
    *ops = (honch_storage_ops_t) {
        .queue_peek = fake_queue_peek,
        .queue_consume = fake_queue_consume,
        .queue_dead_letter = fake_queue_dead_letter,
        .ctx = storage
    };

    *client = (honch_client_t){0};
    assert(pthread_mutex_init(&client->lifetime_mutex, NULL) == 0);
    assert(pthread_cond_init(&client->lifetime_cond, NULL) == 0);
    assert(pthread_mutex_init(&client->mutex, NULL) == 0);
    client->storage = ops;
    storage->client = client;
}

static void fake_client_destroy(honch_client_t *client)
{
    assert(pthread_cond_destroy(&client->lifetime_cond) == 0);
    assert(pthread_mutex_destroy(&client->lifetime_mutex) == 0);
    assert(pthread_mutex_destroy(&client->mutex) == 0);
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

static void test_tiny_buffer_rejected(void)
{
    const uint8_t message[] = {0xa1u, 0x01u, 0x02u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_storage_ops_t ops = {0};
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
}

static void test_single_chunk_message_has_first_and_final_flags(void)
{
    const uint8_t message[] = {0xa1u, 0x65u, 'e', 'v', 'e', 'n', 't'};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_storage_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[64] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);

    assert(out_size == HONCH_PACKET_HEADER_SIZE + sizeof(message));
    assert(complete);
    assert(buffer[0] == 1u);
    assert(buffer[1] == 1u);
    assert(buffer[2] == 0x03u);
    assert(buffer[3] == 0u);
    assert(read_u64_be(buffer + 4u) == HONCH_TEST_SEQUENCE);
    assert(read_u32_be(buffer + 12u) == 0u);
    assert(read_u16_be(buffer + 16u) == sizeof(message));
    assert(read_u16_be(buffer + 18u) == 0xb4c8u);
    assert(memcmp(buffer + HONCH_PACKET_HEADER_SIZE, message, sizeof(message)) == 0);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
}

static void test_multi_chunk_message_offsets_increase(void)
{
    const uint8_t message[] = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_storage_ops_t ops = {0};
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
}

static void test_abort_does_not_consume_storage(void)
{
    const uint8_t message[] = {0xa0u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_storage_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    assert(!storage.consumed);
    assert(storage.has_message);
    assert(!packetizer.active);
    fake_client_destroy(&client);
}

static void test_confirm_consumes_storage(void)
{
    const uint8_t message[] = {0xa0u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true
    };
    honch_storage_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[64] = {0};
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
}

static void test_confirm_failure_invalidates_packetizer_after_releasing_client(void)
{
    const uint8_t message[] = {0xa0u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true,
        .consume_status = HONCH_ERROR_IO
    };
    honch_storage_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};
    uint8_t buffer[64] = {0};
    size_t out_size = 0u;
    bool complete = false;

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(honch_packetizer_next(&packetizer, buffer, sizeof(buffer), &out_size, &complete) == HONCH_OK);
    assert(complete);
    assert(honch_packetizer_confirm(&packetizer) == HONCH_ERROR_IO);
    assert(!packetizer.active);
    assert(packetizer.client == NULL);
    assert(honch_packetizer_abort(&packetizer) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(pthread_mutex_trylock(&client.mutex) == 0);
    assert(pthread_mutex_unlock(&client.mutex) == 0);
    fake_client_destroy(&client);
}

static void test_packetizer_peek_runs_under_client_lock(void)
{
    const uint8_t message[] = {0xa0u};
    fake_storage_t storage = {
        .message = message,
        .message_size = sizeof(message),
        .sequence = HONCH_TEST_SEQUENCE,
        .has_message = true,
        .require_client_lock_on_peek = true
    };
    honch_storage_ops_t ops = {0};
    honch_client_t client = {0};
    fake_client_with_storage(&client, &storage, &ops);
    honch_packetizer_t packetizer = {0};

    assert(honch_packetizer_begin(&client, &packetizer, HONCH_DATA_SOURCE_EVENTS) == HONCH_OK);
    assert(storage.peek_saw_client_lock);
    assert(honch_packetizer_abort(&packetizer) == HONCH_OK);
    fake_client_destroy(&client);
}

int main(void)
{
    test_tiny_buffer_rejected();
    test_single_chunk_message_has_first_and_final_flags();
    test_multi_chunk_message_offsets_increase();
    test_abort_does_not_consume_storage();
    test_confirm_consumes_storage();
    test_confirm_failure_invalidates_packetizer_after_releasing_client();
    test_packetizer_peek_runs_under_client_lock();
    return 0;
}
