#include "honch_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

static honch_status_t fake_post_batch(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const uint8_t *body,
    size_t body_size,
    const char *content_encoding,
    honch_transport_result_t *result)
{
    (void)endpoint_url;
    (void)api_key;
    (void)body;
    (void)body_size;
    (void)content_encoding;
    fake_transport_t *transport = (fake_transport_t *)ctx;
    transport->calls++;
    *result = transport->result;
    return transport->status;
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
        .post_batch = fake_post_batch,
        .ctx = transport
    };

    honch_client_t client = {0};
    client.api_key = "test-key";
    client.endpoint_url = "http://collector.local/";
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
    test_401_dead_letters_events();
    test_400_dead_letters_events();
    test_429_preserves_events();
    test_500_preserves_events();
    test_network_error_preserves_events();
    return 0;
}
