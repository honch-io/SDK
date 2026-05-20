#include "honch_internal.h"

#include <stdlib.h>

static honch_status_t honch_core_queue_depth(honch_client_t *client, size_t *depth)
{
    if (client == NULL || depth == NULL || client->storage == NULL || client->storage->queue_depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_depth(client->storage->ctx, depth);
}

static honch_status_t honch_core_queue_peek(honch_client_t *client, honch_storage_reader_t *reader)
{
    if (client == NULL || reader == NULL || client->storage == NULL || client->storage->queue_peek == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_peek(client->storage->ctx, reader);
}

static honch_status_t honch_core_queue_consume(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_consume == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_consume(client->storage->ctx, sequence);
}

static honch_status_t honch_core_queue_dead_letter(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_dead_letter == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_dead_letter(client->storage->ctx, sequence);
}

static honch_status_t honch_core_read_queue_event(
    honch_client_t *client,
    const honch_storage_reader_t *reader,
    honch_payload_t *event)
{
    event->data = NULL;
    event->length = 0u;

    if (client == NULL || reader == NULL || reader->read == NULL ||
        reader->total_size == 0u || reader->total_size > client->max_event_bytes) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *data = (uint8_t *)malloc(reader->total_size);
    if (data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_status_t status = reader->read(reader->ctx, 0u, data, reader->total_size);
    if (status != HONCH_OK) {
        free(data);
        return status;
    }

    if (!honch_cbor_validate_event(data, reader->total_size)) {
        free(data);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    event->data = data;
    event->length = reader->total_size;
    return HONCH_OK;
}

static honch_status_t honch_core_build_batch(
    honch_client_t *client,
    const honch_payload_t *events,
    size_t event_count,
    honch_payload_t *batch)
{
    batch->data = NULL;
    batch->length = 0u;

    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    if (status == HONCH_OK) {
        status = honch_cbor_append_map(&buffer, 2u);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "token");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, client->api_key);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "batch");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_array(&buffer, event_count);
    }
    for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
        status = honch_buffer_append_n(&buffer, (const char *)events[i].data, events[i].length);
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    batch->data = (unsigned char *)buffer.data;
    batch->length = buffer.length;
    return HONCH_OK;
}

static honch_status_t honch_core_post_batch(
    honch_client_t *client,
    const honch_payload_t *batch,
    honch_transport_result_t *result)
{
    if (client == NULL || batch == NULL || result == NULL ||
        client->transport == NULL || client->transport->post_batch == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *result = HONCH_TRANSPORT_RETRY;
    return client->transport->post_batch(
        client->transport->ctx,
        client->endpoint_url,
        client->api_key,
        batch->data,
        batch->length,
        NULL,
        result);
}

static honch_status_t honch_core_flush_one(honch_client_t *client, bool *progressed)
{
    *progressed = false;

    honch_payload_t events[HONCH_MAX_BATCH_SIZE] = {0};
    uint64_t sequences[HONCH_MAX_BATCH_SIZE] = {0};
    size_t event_count = 0u;
    size_t batch_size = client->batch_size == 0u ? 1u : client->batch_size;
    if (batch_size > HONCH_MAX_BATCH_SIZE) {
        batch_size = HONCH_MAX_BATCH_SIZE;
    }

    client->active_storage_reader_sequence = UINT64_MAX;
    honch_status_t status = HONCH_OK;
    while (event_count < batch_size) {
        honch_storage_reader_t reader = {0};
        status = honch_core_queue_peek(client, &reader);
        if (status == HONCH_ERROR_NOT_INITIALIZED) {
            status = HONCH_OK;
            break;
        }
        if (status != HONCH_OK) {
            break;
        }

        honch_payload_t event = {0};
        status = honch_core_read_queue_event(client, &reader, &event);
        if (status == HONCH_ERROR_INVALID_ARGUMENT) {
            honch_status_t dead_status = honch_core_queue_dead_letter(client, reader.sequence);
            if (dead_status == HONCH_OK) {
                *progressed = true;
                status = HONCH_ERROR_REJECTED;
            } else {
                status = dead_status;
            }
            break;
        }
        if (status != HONCH_OK) {
            break;
        }

        events[event_count] = event;
        sequences[event_count] = reader.sequence;
        event_count++;
    }

    if (status != HONCH_OK || event_count == 0u) {
        for (size_t i = 0u; i < event_count; i++) {
            free(events[i].data);
        }
        return status;
    }

    honch_payload_t batch = {0};
    status = honch_core_build_batch(client, events, event_count, &batch);
    for (size_t i = 0u; i < event_count; i++) {
        free(events[i].data);
    }
    if (status != HONCH_OK) {
        return status;
    }

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    status = honch_core_post_batch(client, &batch, &result);
    free(batch.data);

    if (result == HONCH_TRANSPORT_ACCEPTED && status == HONCH_OK) {
        for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
            status = honch_core_queue_consume(client, sequences[i]);
        }
        *progressed = status == HONCH_OK;
        return status;
    }

    if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
        honch_status_t dead_status = HONCH_OK;
        for (size_t i = 0u; dead_status == HONCH_OK && i < event_count; i++) {
            dead_status = honch_core_queue_dead_letter(client, sequences[i]);
        }
        if (dead_status == HONCH_OK) {
            *progressed = true;
            return status == HONCH_OK ? HONCH_ERROR_REJECTED : status;
        }
        return dead_status;
    }

    return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
}

honch_status_t honch_queue_flush_locked(honch_client_t *client)
{
    honch_status_t final_status = HONCH_OK;

    for (;;) {
        size_t depth = 0u;
        honch_status_t status = honch_core_queue_depth(client, &depth);
        if (status != HONCH_OK) {
            return status;
        }
        client->queued_event_count = depth;
        if (depth == 0u) {
            return final_status;
        }

        bool progressed = false;
        status = honch_core_flush_one(client, &progressed);
        if (status == HONCH_ERROR_REJECTED && progressed) {
            final_status = HONCH_ERROR_REJECTED;
            continue;
        }
        if (status != HONCH_OK) {
            return status;
        }
    }
}
