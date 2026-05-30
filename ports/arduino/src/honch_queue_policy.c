#include "honch_internal.h"
#include "honch/core/wire_v2.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_WIRE_V2_MAX_FRAME_BYTES 4096u

#ifdef HONCH_TESTING
static size_t s_honch_test_max_wire_v2_encode_attempts = 0u;

void honch_test_reset_wire_v2_encode_attempts(void)
{
    s_honch_test_max_wire_v2_encode_attempts = 0u;
}

size_t honch_test_max_wire_v2_encode_attempts(void)
{
    return s_honch_test_max_wire_v2_encode_attempts;
}

#endif

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

static honch_status_t honch_core_queue_consume_batch(
    honch_client_t *client,
    const uint64_t *sequences,
    size_t sequence_count)
{
    if (client == NULL || sequences == NULL || client->storage == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }
    if (client->storage->queue_consume_batch != NULL) {
        return client->storage->queue_consume_batch(client->storage->ctx, sequences, sequence_count);
    }

    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        status = honch_core_queue_consume(client, sequences[i]);
    }
    return status;
}

static honch_status_t honch_core_queue_dead_letter_batch(
    honch_client_t *client,
    const uint64_t *sequences,
    size_t sequence_count)
{
    if (client == NULL || sequences == NULL || client->storage == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }
    if (client->storage->queue_dead_letter_batch != NULL) {
        return client->storage->queue_dead_letter_batch(client->storage->ctx, sequences, sequence_count);
    }

    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        status = honch_core_queue_dead_letter(client, sequences[i]);
    }
    return status;
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

    if (!honch_event_record_validate(data, reader->total_size)) {
        free(data);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    event->data = data;
    event->length = reader->total_size;
    return HONCH_OK;
}

static void honch_core_free_storage_events(honch_storage_event_t *events, size_t event_count)
{
    if (events == NULL) {
        return;
    }
    for (size_t i = 0u; i < event_count; i++) {
        free(events[i].data);
    }
}

static honch_status_t honch_core_read_queue_batch(
    honch_client_t *client,
    honch_payload_t *events,
    uint64_t *sequences,
    size_t batch_size,
    size_t *event_count)
{
    if (event_count == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *event_count = 0u;
    if (client == NULL || events == NULL || sequences == NULL ||
        client->storage == NULL || client->storage->queue_read_batch == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_storage_event_t *storage_events = (honch_storage_event_t *)calloc(batch_size, sizeof(*storage_events));
    if (storage_events == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_status_t status = client->storage->queue_read_batch(
        client->storage->ctx,
        storage_events,
        batch_size,
        client->max_event_bytes,
        event_count);
    if (*event_count > batch_size) {
        honch_core_free_storage_events(storage_events, batch_size);
        free(storage_events);
        *event_count = 0u;
        return HONCH_ERROR_INTERNAL;
    }
    if (status != HONCH_OK) {
        honch_core_free_storage_events(storage_events, *event_count);
        free(storage_events);
        return status;
    }

    for (size_t i = 0u; i < *event_count; i++) {
        if (storage_events[i].data == NULL || storage_events[i].length > client->max_event_bytes) {
            honch_core_free_storage_events(storage_events, *event_count);
            free(storage_events);
            *event_count = i;
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        events[i] = (honch_payload_t) {
            .data = storage_events[i].data,
            .length = storage_events[i].length
        };
        sequences[i] = storage_events[i].sequence;
        storage_events[i].data = NULL;
    }
    free(storage_events);
    return *event_count == 0u ? HONCH_ERROR_NOT_INITIALIZED : HONCH_OK;
}

static honch_status_t honch_core_build_wire_v2_message(
    honch_client_t *client,
    const honch_payload_t *events,
    size_t event_count,
    honch_payload_t *message,
    size_t *encoded_event_count)
{
    message->data = NULL;
    message->length = 0u;

    if (event_count == 0u || encoded_event_count == NULL ||
        client->device_id == NULL || client->device_model == NULL ||
        client->firmware_version == NULL || client->sdk_platform == NULL || client->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *encoded_event_count = 0u;

    honch_event_record_t *parsed = (honch_event_record_t *)calloc(event_count, sizeof(*parsed));
    if (parsed == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_wire_v2_event_t *compact_events = (honch_wire_v2_event_t *)calloc(event_count, sizeof(*compact_events));
    if (compact_events == NULL) {
        free(parsed);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_status_t status = HONCH_OK;
    size_t parsed_count = 0u;
    for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
        status = honch_event_record_parse(events[i].data, events[i].length, &parsed[i]);
        if (status != HONCH_OK) {
            *encoded_event_count = i;
            break;
        }
        honch_event_record_prepare_wire_properties(&parsed[i]);
        if (status == HONCH_OK && i > 0u && strcmp(parsed[i].distinct_id, parsed[0].distinct_id) != 0) {
            break;
        }
        if (status == HONCH_OK && i > 0u &&
            ((parsed[i].session_id == NULL) != (parsed[0].session_id == NULL) ||
                (parsed[i].session_id != NULL && strcmp(parsed[i].session_id, parsed[0].session_id) != 0))) {
            break;
        }
        if (status == HONCH_OK) {
            compact_events[i] = (honch_wire_v2_event_t) {
                .event_name = parsed[i].event_name,
                .timestamp_ms = parsed[i].timestamp_ms,
                .properties = parsed[i].properties,
                .property_count = parsed[i].property_count
            };
            parsed_count = i + 1u;
        }
    }
    if (status != HONCH_OK || parsed_count == 0u) {
        for (size_t i = 0u; i < event_count; i++) {
            honch_event_record_free(&parsed[i]);
        }
        free(compact_events);
        free(parsed);
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }

    honch_wire_v2_batch_context_t context = {
        .distinct_id = parsed[0].distinct_id,
        .device_id = client->device_id,
        .device_model = client->device_model,
        .firmware_version = client->firmware_version,
        .sdk_platform = client->sdk_platform,
        .sdk_version = HONCH_SDK_VERSION,
        .environment = client->environment,
        .session_id = parsed[0].session_id != NULL ? parsed[0].session_id : client->session_id
    };

    uint8_t *buffer = (uint8_t *)malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (buffer == NULL) {
        for (size_t i = 0u; i < event_count; i++) {
            honch_event_record_free(&parsed[i]);
        }
        free(compact_events);
        free(parsed);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    size_t message_size = 0u;
    size_t encoded_count = 0u;
    size_t encode_attempts = 0u;
    status = HONCH_ERROR_OUT_OF_MEMORY;
    size_t low = 1u;
    size_t high = parsed_count;
    while (low <= high) {
        size_t try_count = low + ((high - low) / 2u);
        encode_attempts++;
        status = honch_wire_v2_encode_event_batch(
            &context,
            parsed[0].timestamp_ms,
            compact_events,
            try_count,
            buffer,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &message_size);
        if (status == HONCH_OK) {
            encoded_count = try_count;
            low = try_count + 1u;
            continue;
        }
        if (status != HONCH_ERROR_OUT_OF_MEMORY) {
            break;
        }
        if (try_count == 1u) {
            break;
        }
        high = try_count - 1u;
    }
    if (encoded_count > 0u) {
        encode_attempts++;
        status = honch_wire_v2_encode_event_batch(
            &context,
            parsed[0].timestamp_ms,
            compact_events,
            encoded_count,
            buffer,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &message_size);
    } else if (status == HONCH_ERROR_OUT_OF_MEMORY) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
#ifdef HONCH_TESTING
    if (encode_attempts > s_honch_test_max_wire_v2_encode_attempts) {
        s_honch_test_max_wire_v2_encode_attempts = encode_attempts;
    }
#else
    (void)encode_attempts;
#endif
    for (size_t i = 0u; i < event_count; i++) {
        honch_event_record_free(&parsed[i]);
    }
    free(compact_events);
    free(parsed);
    if (status != HONCH_OK) {
        free(buffer);
        return status;
    }

    message->data = buffer;
    message->length = message_size;
    *encoded_event_count = encoded_count;
    return HONCH_OK;
}

static honch_status_t honch_core_post_wire_v2_message(
    honch_client_t *client,
    const honch_payload_t *message,
    uint32_t message_id,
    const char *stream_id,
    honch_transport_result_t *result)
{
    if (client == NULL || message == NULL || result == NULL ||
        client->transport == NULL || client->transport->post_chunk == NULL ||
        stream_id == NULL || stream_id[0] == '\0') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *frame = (uint8_t *)malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (frame == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_wire_v2_chunker_t chunker = {0};
    honch_status_t status = honch_wire_v2_chunker_begin(
        &chunker,
        message_id,
        message->data,
        message->length,
        HONCH_WIRE_V2_SOURCE_EVENTS,
        HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (status != HONCH_OK) {
        free(frame);
        return status;
    }

    *result = HONCH_TRANSPORT_RETRY;
    bool complete = false;
    while (!complete) {
        size_t frame_size = 0u;
        status = honch_wire_v2_chunker_next(
            &chunker,
            frame,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &frame_size,
            &complete);
        if (status != HONCH_OK) {
            free(frame);
            return status;
        }
        status = client->transport->post_chunk(
            client->transport->ctx,
            client->endpoint_url,
            client->api_key,
            stream_id,
            frame,
            frame_size,
            result);
        if (status != HONCH_OK) {
            free(frame);
            return status;
        }
        if (complete) {
            if (*result == HONCH_TRANSPORT_ACCEPTED) {
                break;
            }
            if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
                free(frame);
                return HONCH_OK;
            }
            *result = HONCH_TRANSPORT_RETRY;
            free(frame);
            return HONCH_ERROR_TRANSPORT;
        }
        if (*result == HONCH_TRANSPORT_CHUNK_STORED) {
            continue;
        }
        if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
            free(frame);
            return HONCH_OK;
        }
        *result = HONCH_TRANSPORT_RETRY;
        free(frame);
        return HONCH_ERROR_TRANSPORT;
    }

    free(frame);
    return HONCH_OK;
}

static honch_status_t honch_core_flush_one(honch_client_t *client, bool *progressed)
{
    *progressed = false;

    size_t batch_size = client->batch_size == 0u ? 1u : client->batch_size;
    if (batch_size > HONCH_MAX_BATCH_SIZE) {
        batch_size = HONCH_MAX_BATCH_SIZE;
    }
    honch_payload_t *events = (honch_payload_t *)calloc(batch_size, sizeof(*events));
    uint64_t *sequences = (uint64_t *)calloc(batch_size, sizeof(*sequences));
    if (events == NULL || sequences == NULL) {
        free(events);
        free(sequences);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    size_t event_count = 0u;

    honch_status_t status = HONCH_OK;
    bool use_reader_path = client->storage == NULL || client->storage->queue_read_batch == NULL;
    if (client->storage != NULL && client->storage->queue_read_batch != NULL) {
        status = honch_core_read_queue_batch(client, events, sequences, batch_size, &event_count);
        if (status == HONCH_ERROR_NOT_INITIALIZED) {
            status = HONCH_OK;
        } else if (status == HONCH_ERROR_INVALID_ARGUMENT) {
            event_count = 0u;
            status = HONCH_OK;
            use_reader_path = true;
        }
    }
    if (use_reader_path) {
        client->active_storage_reader_sequence = UINT64_MAX;
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
    }

    if (status != HONCH_OK || event_count == 0u) {
        for (size_t i = 0u; i < event_count; i++) {
            free(events[i].data);
        }
        free(events);
        free(sequences);
        return status;
    }

    honch_payload_t compact_message = {0};
    size_t encoded_event_count = event_count;
    if (client->transport == NULL || client->transport->post_chunk == NULL) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    } else {
        status = honch_core_build_wire_v2_message(
            client,
            events,
            event_count,
            &compact_message,
            &encoded_event_count);
    }
    for (size_t i = 0u; i < event_count; i++) {
        free(events[i].data);
    }
    if (status != HONCH_OK) {
        if (status == HONCH_ERROR_INVALID_ARGUMENT && encoded_event_count < event_count) {
            honch_status_t dead_status = honch_core_queue_dead_letter(client, sequences[encoded_event_count]);
            if (dead_status == HONCH_OK) {
                *progressed = true;
                free(events);
                free(sequences);
                return HONCH_ERROR_REJECTED;
            }
            free(events);
            free(sequences);
            return dead_status;
        }
        free(events);
        free(sequences);
        return status;
    }
    event_count = encoded_event_count;

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    uint32_t message_id = client->wire_v2_message_id_seed + (uint32_t)sequences[0];
    status = honch_core_post_wire_v2_message(
        client,
        &compact_message,
        message_id,
        client->wire_v2_stream_id,
        &result);
    free(compact_message.data);

    if (result == HONCH_TRANSPORT_ACCEPTED && status == HONCH_OK) {
        status = honch_core_queue_consume_batch(client, sequences, event_count);
        *progressed = status == HONCH_OK;
        free(events);
        free(sequences);
        return status;
    }

    if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
        honch_status_t dead_status = honch_core_queue_dead_letter_batch(client, sequences, event_count);
        if (dead_status == HONCH_OK) {
            *progressed = true;
            free(events);
            free(sequences);
            return status == HONCH_OK ? HONCH_ERROR_REJECTED : status;
        }
        free(events);
        free(sequences);
        return dead_status;
    }

    free(events);
    free(sequences);
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
