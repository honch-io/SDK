#include "honch_internal.h"
#include "honch/core/wire_v2.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct honch_flush_context {
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *sdk_platform;
    const char *environment;
    const char *session_id;
    uint32_t message_id_seed;
    char stream_id[9];
    bool has_boot_epoch_ms;
    uint64_t boot_epoch_ms;
    uint64_t flush_uptime_ms;
} honch_flush_context_t;

#ifdef HONCH_FLUSH_TIMING
static uint64_t honch_flush_timing_now_ms(honch_client_t *client)
{
    if (client == NULL || client->platform == NULL || client->platform->uptime_ms == NULL) {
        return 0u;
    }
    return client->platform->uptime_ms(client->platform->ctx);
}

static void honch_flush_timing_log(honch_client_t *client, const char *message)
{
    if (client == NULL || client->platform == NULL || client->platform->log == NULL) {
        return;
    }
    client->platform->log(client->platform->ctx, HONCH_LOG_INFO, message);
}
#endif

static size_t s_honch_test_max_wire_v2_encode_attempts = 0u;

void honch_test_reset_wire_v2_encode_attempts(void)
{
    s_honch_test_max_wire_v2_encode_attempts = 0u;
}

size_t honch_test_max_wire_v2_encode_attempts(void)
{
    return s_honch_test_max_wire_v2_encode_attempts;
}

void honch_core_wire_v2_context_from_client(
    honch_client_t *client,
    honch_wire_v2_encode_context_t *context)
{
    if (context == NULL) {
        return;
    }
    *context = (honch_wire_v2_encode_context_t){0};
    if (client == NULL) {
        return;
    }

    context->device_id = client->device_id;
    context->device_model = client->device_model;
    context->firmware_version = client->firmware_version;
    context->sdk_platform = client->sdk_platform;
    context->environment = client->environment;
    context->session_id = client->session_id;

    if (client->platform == NULL ||
        client->platform->now_ms == NULL || client->platform->uptime_ms == NULL) {
        return;
    }

    uint64_t now_ms = client->platform->now_ms(client->platform->ctx);
    uint64_t uptime_ms = client->platform->uptime_ms(client->platform->ctx);
    if (now_ms < HONCH_MIN_UNIX_TIME_MS || uptime_ms == 0u || now_ms <= uptime_ms) {
        return;
    }

    context->has_boot_epoch_ms = true;
    context->boot_epoch_ms = now_ms - uptime_ms;
    context->flush_uptime_ms = uptime_ms;
}

uint64_t honch_core_normalize_wire_v2_timestamp(
    const honch_wire_v2_encode_context_t *context,
    uint64_t timestamp_ms)
{
    if (context == NULL || !context->has_boot_epoch_ms ||
        timestamp_ms == 0u || timestamp_ms >= HONCH_MIN_UNIX_TIME_MS ||
        timestamp_ms > context->flush_uptime_ms ||
        UINT64_MAX - context->boot_epoch_ms < timestamp_ms) {
        return timestamp_ms;
    }

    return context->boot_epoch_ms + timestamp_ms;
}

static uint64_t honch_device_time_source_for_event(uint64_t queued_timestamp_ms, uint64_t normalized_timestamp_ms)
{
    if (normalized_timestamp_ms < HONCH_MIN_UNIX_TIME_MS) {
        return 0u;
    }
    if (queued_timestamp_ms > 0u &&
        queued_timestamp_ms < HONCH_MIN_UNIX_TIME_MS &&
        normalized_timestamp_ms >= HONCH_MIN_UNIX_TIME_MS) {
        return 2u;
    }
    return 1u;
}

static uint64_t honch_device_time_source_for_batch(bool has_unknown_time, bool used_boot_epoch)
{
    if (has_unknown_time) {
        return 0u;
    }
    return used_boot_epoch ? 2u : 1u;
}

static honch_status_t honch_core_queue_depth(honch_client_t *client, size_t *depth)
{
    if (client == NULL || depth == NULL || client->event_queue == NULL || client->event_queue->queue_depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->event_queue->queue_depth(client->event_queue->ctx, depth);
}

static honch_status_t honch_core_queue_peek(honch_client_t *client, honch_storage_reader_t *reader)
{
    if (client == NULL || reader == NULL || client->event_queue == NULL || client->event_queue->queue_peek == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->event_queue->queue_peek(client->event_queue->ctx, reader);
}

static honch_status_t honch_core_queue_consume(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->event_queue == NULL || client->event_queue->queue_consume == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->event_queue->queue_consume(client->event_queue->ctx, sequence);
}

static honch_status_t honch_core_queue_dead_letter(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->event_queue == NULL || client->event_queue->queue_dead_letter == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->event_queue->queue_dead_letter(client->event_queue->ctx, sequence);
}

static honch_status_t honch_core_queue_consume_batch(
    honch_client_t *client,
    const uint64_t *sequences,
    size_t sequence_count)
{
    if (client == NULL || sequences == NULL || client->event_queue == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }
    if (client->event_queue->queue_consume_batch != NULL) {
        return client->event_queue->queue_consume_batch(client->event_queue->ctx, sequences, sequence_count);
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
    if (client == NULL || sequences == NULL || client->event_queue == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }
    if (client->event_queue->queue_dead_letter_batch != NULL) {
        return client->event_queue->queue_dead_letter_batch(client->event_queue->ctx, sequences, sequence_count);
    }

    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        status = honch_core_queue_dead_letter(client, sequences[i]);
    }
    return status;
}

static honch_status_t honch_flush_context_snapshot(honch_client_t *client, honch_flush_context_t *context)
{
    if (client == NULL || context == NULL ||
        client->device_id == NULL || client->device_model == NULL ||
        client->firmware_version == NULL || client->sdk_platform == NULL || client->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *context = (honch_flush_context_t){0};
    context->device_id = client->device_id;
    context->device_model = client->device_model;
    context->firmware_version = client->firmware_version;
    context->sdk_platform = client->sdk_platform;
    context->environment = client->environment;
    context->session_id = client->session_id;
    context->message_id_seed = client->wire_v2_message_id_seed;
    memcpy(context->stream_id, client->wire_v2_stream_id, sizeof(context->stream_id));
    honch_wire_v2_encode_context_t borrowed_context = {0};
    honch_core_wire_v2_context_from_client(client, &borrowed_context);
    context->has_boot_epoch_ms = borrowed_context.has_boot_epoch_ms;
    context->boot_epoch_ms = borrowed_context.boot_epoch_ms;
    context->flush_uptime_ms = borrowed_context.flush_uptime_ms;

    return HONCH_OK;
}

static void honch_flush_context_free(honch_flush_context_t *context)
{
    if (context == NULL) {
        return;
    }
    *context = (honch_flush_context_t){0};
}

static void honch_core_free_flush_events(honch_client_t *client, size_t event_count)
{
    if (client == NULL) {
        return;
    }
    for (size_t i = 0u; i < event_count; i++) {
        if (!client->flush_event_borrowed[i]) {
            free(client->flush_events[i].data);
        }
        client->flush_events[i] = (honch_payload_t){0};
        client->flush_event_borrowed[i] = false;
    }
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
        if ((events[i].flags & HONCH_STORAGE_EVENT_BORROWED) == 0u) {
            free(events[i].data);
        }
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
        client->event_queue == NULL || client->event_queue->queue_read_batch == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_storage_event_t *storage_events = client->flush_storage_events;
    memset(storage_events, 0, batch_size * sizeof(*storage_events));
    memset(client->flush_event_borrowed, 0, batch_size * sizeof(client->flush_event_borrowed[0]));

    honch_status_t status = client->event_queue->queue_read_batch(
        client->event_queue->ctx,
        storage_events,
        batch_size,
        client->max_event_bytes,
        event_count);
    if (*event_count > batch_size) {
        honch_core_free_storage_events(storage_events, batch_size);
        *event_count = 0u;
        return HONCH_ERROR_INTERNAL;
    }
    if (status != HONCH_OK) {
        honch_core_free_storage_events(storage_events, *event_count);
        return status;
    }

    for (size_t i = 0u; i < *event_count; i++) {
        if (storage_events[i].data == NULL || storage_events[i].length > client->max_event_bytes) {
            honch_core_free_storage_events(storage_events, *event_count);
            *event_count = i;
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        events[i] = (honch_payload_t) {
            .data = storage_events[i].data,
            .length = storage_events[i].length
        };
        client->flush_event_borrowed[i] = (storage_events[i].flags & HONCH_STORAGE_EVENT_BORROWED) != 0u;
        sequences[i] = storage_events[i].sequence;
        storage_events[i].data = NULL;
    }
    return *event_count == 0u ? HONCH_ERROR_NOT_INITIALIZED : HONCH_OK;
}

honch_status_t honch_core_encode_single_wire_v2_event(
    const honch_wire_v2_encode_context_t *context,
    const honch_payload_t *event,
    uint8_t *buffer,
    size_t buffer_capacity,
    honch_payload_t *message)
{
    if (message == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    message->data = NULL;
    message->length = 0u;

    if (context == NULL || event == NULL || event->data == NULL || event->length == 0u ||
        buffer == NULL || buffer_capacity == 0u ||
        context->device_id == NULL || context->device_model == NULL ||
        context->firmware_version == NULL || context->sdk_platform == NULL ||
        context->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_event_record_t record = {0};
    honch_status_t status = honch_event_record_parse(event->data, event->length, &record);
    if (status != HONCH_OK) {
        return status;
    }

    honch_event_record_prepare_wire_properties(&record);
    uint64_t queued_timestamp_ms = record.timestamp_ms;
    record.timestamp_ms = honch_core_normalize_wire_v2_timestamp(context, record.timestamp_ms);

    honch_wire_v2_event_t compact_event = {
        .event_name = record.event_name,
        .timestamp_ms = record.timestamp_ms,
        .properties = record.properties,
        .property_count = record.property_count
    };
    honch_wire_v2_batch_context_t batch_context = {
        .distinct_id = record.distinct_id,
        .device_id = context->device_id,
        .device_model = context->device_model,
        .firmware_version = context->firmware_version,
        .sdk_platform = context->sdk_platform,
        .sdk_version = HONCH_SDK_VERSION,
        .environment = context->environment,
        .session_id = record.session_id != NULL ? record.session_id : context->session_id,
        .has_device_time_source = true,
        .device_time_source = honch_device_time_source_for_event(queued_timestamp_ms, record.timestamp_ms)
    };

    size_t message_size = 0u;
    status = honch_wire_v2_encode_event_batch(
        &batch_context,
        record.timestamp_ms,
        &compact_event,
        1u,
        buffer,
        buffer_capacity,
        &message_size);
    honch_event_record_free(&record);
    if (status != HONCH_OK) {
        return status;
    }

    message->data = buffer;
    message->length = message_size;
    return HONCH_OK;
}

typedef struct honch_flush_wire_provider_context {
    const honch_payload_t *events;
    const honch_wire_v2_encode_context_t *encode_context;
    honch_event_record_t *record;
    bool record_active;
} honch_flush_wire_provider_context_t;

static void honch_flush_wire_provider_clear(honch_flush_wire_provider_context_t *ctx)
{
    if (ctx == NULL || ctx->record == NULL || !ctx->record_active) {
        return;
    }
    honch_event_record_free(ctx->record);
    memset(ctx->record, 0, sizeof(*ctx->record));
    ctx->record_active = false;
}

static honch_status_t honch_flush_wire_event_provider(
    void *ctx,
    size_t index,
    honch_wire_v2_event_t *event)
{
    honch_flush_wire_provider_context_t *provider_ctx = (honch_flush_wire_provider_context_t *)ctx;
    if (provider_ctx == NULL || provider_ctx->events == NULL ||
        provider_ctx->encode_context == NULL || provider_ctx->record == NULL || event == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_flush_wire_provider_clear(provider_ctx);
    honch_status_t status = honch_event_record_parse(
        provider_ctx->events[index].data,
        provider_ctx->events[index].length,
        provider_ctx->record);
    if (status != HONCH_OK) {
        return status;
    }
    provider_ctx->record_active = true;

    honch_event_record_prepare_wire_properties(provider_ctx->record);
    provider_ctx->record->timestamp_ms = honch_core_normalize_wire_v2_timestamp(
        provider_ctx->encode_context,
        provider_ctx->record->timestamp_ms);

    *event = (honch_wire_v2_event_t) {
        .event_name = provider_ctx->record->event_name,
        .timestamp_ms = provider_ctx->record->timestamp_ms,
        .properties = provider_ctx->record->properties,
        .property_count = provider_ctx->record->property_count
    };
    return HONCH_OK;
}

static honch_status_t honch_flush_measure_wire_v2_batch(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    honch_flush_wire_provider_context_t *provider_ctx,
    size_t event_count,
    size_t *message_size)
{
    honch_status_t status = honch_wire_v2_measure_event_batch_provider(
        context,
        base_time_ms,
        honch_flush_wire_event_provider,
        provider_ctx,
        event_count,
        message_size);
    honch_flush_wire_provider_clear(provider_ctx);
    return status;
}

static honch_status_t honch_flush_encode_wire_v2_batch(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    honch_flush_wire_provider_context_t *provider_ctx,
    size_t event_count,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *message_size)
{
    honch_status_t status = honch_wire_v2_encode_event_batch_provider(
        context,
        base_time_ms,
        honch_flush_wire_event_provider,
        provider_ctx,
        event_count,
        buffer,
        buffer_capacity,
        message_size);
    honch_flush_wire_provider_clear(provider_ctx);
    return status;
}

static honch_status_t honch_core_build_wire_v2_message(
    honch_client_t *client,
    const honch_flush_context_t *flush_context,
    const honch_payload_t *events,
    size_t event_count,
    honch_payload_t *message,
    size_t *encoded_event_count)
{
    message->data = NULL;
    message->length = 0u;

    if (client == NULL || event_count == 0u || event_count > HONCH_FLUSH_SCRATCH_MAX_EVENTS ||
        encoded_event_count == NULL ||
        flush_context == NULL || flush_context->device_id == NULL || flush_context->device_model == NULL ||
        flush_context->firmware_version == NULL || flush_context->sdk_platform == NULL ||
        flush_context->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *encoded_event_count = 0u;

    honch_wire_v2_encode_context_t encode_context = {
        .device_id = flush_context->device_id,
        .device_model = flush_context->device_model,
        .firmware_version = flush_context->firmware_version,
        .sdk_platform = flush_context->sdk_platform,
        .environment = flush_context->environment,
        .session_id = flush_context->session_id,
        .has_boot_epoch_ms = flush_context->has_boot_epoch_ms,
        .boot_epoch_ms = flush_context->boot_epoch_ms,
        .flush_uptime_ms = flush_context->flush_uptime_ms
    };
    if (event_count == 1u) {
        honch_status_t status = honch_core_encode_single_wire_v2_event(
            &encode_context,
            &events[0],
            client->flush_message_buffer,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            message);
        if (status == HONCH_OK) {
            *encoded_event_count = 1u;
        }
        if (status == HONCH_ERROR_OUT_OF_MEMORY) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        }
        return status;
    }

    honch_event_record_t *parsed = &client->flush_parsed_record;
    memset(parsed, 0, sizeof(*parsed));

    honch_status_t status = HONCH_OK;
    size_t parsed_count = 0u;
    bool used_boot_epoch = false;
    bool has_unknown_time = false;
    bool first_has_session_id = false;
    char *batch_distinct_id = NULL;
    char *batch_session_id = NULL;
    uint64_t base_time_ms = 0u;
    for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
        honch_event_record_free(parsed);
        memset(parsed, 0, sizeof(*parsed));
        status = honch_event_record_parse(events[i].data, events[i].length, parsed);
        if (status != HONCH_OK) {
            *encoded_event_count = i;
            break;
        }
        honch_event_record_prepare_wire_properties(parsed);
        if (i == 0u) {
            batch_distinct_id = honch_strdup(parsed->distinct_id);
            if (batch_distinct_id == NULL) {
                status = HONCH_ERROR_OUT_OF_MEMORY;
                break;
            }
            first_has_session_id = parsed->session_id != NULL;
            if (first_has_session_id) {
                batch_session_id = honch_strdup(parsed->session_id);
                if (batch_session_id == NULL) {
                    status = HONCH_ERROR_OUT_OF_MEMORY;
                    break;
                }
            }
        }
        if (status == HONCH_OK && i > 0u && strcmp(parsed->distinct_id, batch_distinct_id) != 0) {
            break;
        }
        if (status == HONCH_OK && i > 0u &&
            ((parsed->session_id != NULL) != first_has_session_id ||
                (parsed->session_id != NULL && strcmp(parsed->session_id, batch_session_id) != 0))) {
            break;
        }
        if (status == HONCH_OK) {
            uint64_t queued_timestamp_ms = parsed->timestamp_ms;
            parsed->timestamp_ms = honch_core_normalize_wire_v2_timestamp(&encode_context, parsed->timestamp_ms);
            if (queued_timestamp_ms > 0u &&
                queued_timestamp_ms < HONCH_MIN_UNIX_TIME_MS &&
                parsed->timestamp_ms >= HONCH_MIN_UNIX_TIME_MS) {
                used_boot_epoch = true;
            }
            if (parsed->timestamp_ms < HONCH_MIN_UNIX_TIME_MS) {
                has_unknown_time = true;
            }
            if (i == 0u) {
                base_time_ms = parsed->timestamp_ms;
            }
            parsed_count = i + 1u;
        }
    }
    honch_event_record_free(parsed);
    memset(parsed, 0, sizeof(*parsed));
    if (status != HONCH_OK || parsed_count == 0u) {
        free(batch_distinct_id);
        free(batch_session_id);
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }

    honch_wire_v2_batch_context_t context = {
        .distinct_id = batch_distinct_id,
        .device_id = flush_context->device_id,
        .device_model = flush_context->device_model,
        .firmware_version = flush_context->firmware_version,
        .sdk_platform = flush_context->sdk_platform,
        .sdk_version = HONCH_SDK_VERSION,
        .environment = flush_context->environment,
        .session_id = first_has_session_id ? batch_session_id : flush_context->session_id,
        .has_device_time_source = true,
        .device_time_source = honch_device_time_source_for_batch(has_unknown_time, used_boot_epoch)
    };
    honch_flush_wire_provider_context_t provider_ctx = {
        .events = events,
        .encode_context = &encode_context,
        .record = parsed
    };

    uint8_t *buffer = client->flush_message_buffer;

    size_t message_size = 0u;
    size_t encoded_count = 0u;
    size_t encode_attempts = 0u;
    status = honch_flush_measure_wire_v2_batch(
        &context,
        base_time_ms,
        &provider_ctx,
        parsed_count,
        &message_size);
    if (status == HONCH_OK && message_size <= HONCH_WIRE_V2_MAX_FRAME_BYTES) {
        encoded_count = parsed_count;
    } else if (status == HONCH_ERROR_OUT_OF_MEMORY || status == HONCH_ERROR_INVALID_ARGUMENT) {
        status = HONCH_ERROR_OUT_OF_MEMORY;
        size_t low = 1u;
        size_t high = parsed_count;
        while (low <= high) {
            size_t try_count = low + ((high - low) / 2u);
            status = honch_flush_measure_wire_v2_batch(
                &context,
                base_time_ms,
                &provider_ctx,
                try_count,
                &message_size);
            if (status == HONCH_OK && message_size <= HONCH_WIRE_V2_MAX_FRAME_BYTES) {
                encoded_count = try_count;
                low = try_count + 1u;
                continue;
            }
            if (status != HONCH_ERROR_OUT_OF_MEMORY && status != HONCH_ERROR_INVALID_ARGUMENT) {
                break;
            }
            if (try_count == 1u) {
                break;
            }
            high = try_count - 1u;
        }
    }
    if (encoded_count > 0u) {
        encode_attempts++;
        status = honch_flush_encode_wire_v2_batch(
            &context,
            base_time_ms,
            &provider_ctx,
            encoded_count,
            buffer,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &message_size);
    } else if (status == HONCH_ERROR_OUT_OF_MEMORY) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (encode_attempts > s_honch_test_max_wire_v2_encode_attempts) {
        s_honch_test_max_wire_v2_encode_attempts = encode_attempts;
    }
    honch_flush_wire_provider_clear(&provider_ctx);
    free(batch_distinct_id);
    free(batch_session_id);
    if (status != HONCH_OK) {
        return status;
    }

    message->data = buffer;
    message->length = message_size;
    *encoded_event_count = encoded_count;
    return HONCH_OK;
}

static void honch_pending_flush_clear(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }
    client->pending_flush_active = false;
    client->pending_flush_message_size = 0u;
    client->pending_flush_message_offset = 0u;
    client->pending_flush_event_count = 0u;
    client->pending_flush_message_id = 0u;
    client->pending_flush_stream_id[0] = '\0';
}

static honch_status_t honch_core_post_wire_v2_message_limited(
    honch_client_t *client,
    const honch_payload_t *message,
    uint32_t message_id,
    const char *stream_id,
    size_t max_chunks,
    size_t *message_offset,
    bool *message_complete,
    honch_transport_result_t *result)
{
    if (client == NULL || message == NULL || result == NULL ||
        client->transport == NULL || client->transport->post_chunk == NULL ||
        stream_id == NULL || stream_id[0] == '\0' ||
        max_chunks == 0u || message_offset == NULL || message_complete == NULL ||
        *message_offset >= message->length) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *frame = client->flush_frame_buffer;

    honch_wire_v2_chunker_t chunker = {0};
    honch_status_t status = honch_wire_v2_chunker_begin(
        &chunker,
        message_id,
        message->data,
        message->length,
        HONCH_WIRE_V2_SOURCE_EVENTS,
        HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (status != HONCH_OK) {
        return status;
    }
    chunker.offset = *message_offset;

    *result = HONCH_TRANSPORT_RETRY;
    bool complete = false;
    *message_complete = false;
    size_t chunks_posted = 0u;
#ifdef HONCH_FLUSH_TIMING
    size_t chunk_count = 0u;
#endif
    while (!complete && chunks_posted < max_chunks) {
        size_t frame_size = 0u;
        size_t chunk_offset = chunker.offset;
        status = honch_wire_v2_chunker_next(
            &chunker,
            frame,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &frame_size,
            &complete);
        if (status != HONCH_OK) {
            return status;
        }
#ifdef HONCH_FLUSH_TIMING
        uint64_t post_start_ms = honch_flush_timing_now_ms(client);
#endif
        status = client->transport->post_chunk(
            client->transport->ctx,
            client->endpoint_url,
            client->api_key,
            stream_id,
            frame,
            frame_size,
            result);
#ifdef HONCH_FLUSH_TIMING
        uint64_t post_elapsed_ms = honch_flush_timing_now_ms(client) - post_start_ms;
        chunk_count++;
        char timing_message[192];
        snprintf(
            timing_message,
            sizeof(timing_message),
            "HONCH_FLUSH_TIMING phase=post_chunk chunk=%u frame_bytes=%u complete=%u status=%d result=%d elapsed_ms=%llu",
            (unsigned)chunk_count,
            (unsigned)frame_size,
            complete ? 1u : 0u,
            (int)status,
            (int)*result,
            (unsigned long long)post_elapsed_ms);
        honch_flush_timing_log(client, timing_message);
#endif
        if (status != HONCH_OK) {
            return status;
        }
        chunks_posted++;
        if (complete) {
            if (*result == HONCH_TRANSPORT_ACCEPTED) {
                *message_offset = chunker.offset;
                *message_complete = true;
                break;
            }
            if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
                *message_offset = chunker.offset;
                *message_complete = true;
                return HONCH_OK;
            }
            chunker.offset = chunk_offset;
            *result = HONCH_TRANSPORT_RETRY;
            return HONCH_ERROR_TRANSPORT;
        }
        if (*result == HONCH_TRANSPORT_CHUNK_STORED) {
            *message_offset = chunker.offset;
            continue;
        }
        if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
            *message_offset = chunker.offset;
            *message_complete = true;
            return HONCH_OK;
        }
        chunker.offset = chunk_offset;
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    return HONCH_OK;
}

static honch_status_t honch_queue_post_pending_flush_locked(
    honch_client_t *client,
    size_t max_chunks,
    bool *progressed)
{
    if (client == NULL || progressed == NULL || !client->pending_flush_active) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *progressed = false;

    honch_payload_t compact_message = {
        .data = client->flush_message_buffer,
        .length = client->pending_flush_message_size
    };
    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    bool complete = false;
    honch_status_t status = honch_core_post_wire_v2_message_limited(
        client,
        &compact_message,
        client->pending_flush_message_id,
        client->pending_flush_stream_id,
        max_chunks,
        &client->pending_flush_message_offset,
        &complete,
        &result);
    client->outbound_upload_attempted = true;
    if (status != HONCH_OK) {
        if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
            honch_status_t dead_status = honch_core_queue_dead_letter_batch(
                client,
                client->flush_sequences,
                client->pending_flush_event_count);
            if (dead_status == HONCH_OK) {
                honch_pending_flush_clear(client);
                *progressed = true;
                return status;
            }
            return dead_status;
        }
        return status;
    }
    if (!complete) {
        client->scheduler_flush_requested = true;
        return HONCH_OK;
    }

    if (result == HONCH_TRANSPORT_ACCEPTED) {
        status = honch_core_queue_consume_batch(
            client,
            client->flush_sequences,
            client->pending_flush_event_count);
        *progressed = status == HONCH_OK;
        if (status == HONCH_OK) {
            honch_pending_flush_clear(client);
        }
        return status;
    }

    if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
        honch_status_t dead_status = honch_core_queue_dead_letter_batch(
            client,
            client->flush_sequences,
            client->pending_flush_event_count);
        if (dead_status == HONCH_OK) {
            honch_pending_flush_clear(client);
            *progressed = true;
            return status == HONCH_OK ? HONCH_ERROR_REJECTED : status;
        }
        return dead_status;
    }

    return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
}

static honch_status_t honch_queue_flush_one_with_chunk_limit_locked(
    honch_client_t *client,
    size_t max_chunks,
    bool *progressed)
{
    *progressed = false;
    if (client->pending_flush_active) {
        return honch_queue_post_pending_flush_locked(client, max_chunks, progressed);
    }

    size_t batch_size = client->batch_size == 0u ? 1u : client->batch_size;
    if (batch_size > HONCH_MAX_BATCH_SIZE) {
        batch_size = HONCH_MAX_BATCH_SIZE;
    }
    if (batch_size > HONCH_FLUSH_SCRATCH_MAX_EVENTS) {
        batch_size = HONCH_FLUSH_SCRATCH_MAX_EVENTS;
    }
    honch_payload_t *events = client->flush_events;
    uint64_t *sequences = client->flush_sequences;
    memset(events, 0, batch_size * sizeof(*events));
    memset(sequences, 0, batch_size * sizeof(*sequences));
    size_t event_count = 0u;

    honch_status_t status = HONCH_OK;
    bool use_reader_path = client->event_queue == NULL || client->event_queue->queue_read_batch == NULL;
#ifdef HONCH_FLUSH_TIMING
    uint64_t read_start_ms = honch_flush_timing_now_ms(client);
#endif
    if (client->event_queue != NULL && client->event_queue->queue_read_batch != NULL) {
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
            client->flush_event_borrowed[event_count] = false;
            sequences[event_count] = reader.sequence;
            event_count++;
        }
    }
#ifdef HONCH_FLUSH_TIMING
    uint64_t read_elapsed_ms = honch_flush_timing_now_ms(client) - read_start_ms;
    char timing_message[224];
    snprintf(
        timing_message,
        sizeof(timing_message),
        "HONCH_FLUSH_TIMING phase=read_batch batch_size=%u events=%u status=%d reader_path=%u elapsed_ms=%llu",
        (unsigned)batch_size,
        (unsigned)event_count,
        (int)status,
        use_reader_path ? 1u : 0u,
        (unsigned long long)read_elapsed_ms);
    honch_flush_timing_log(client, timing_message);
#endif

    if (status != HONCH_OK || event_count == 0u) {
        honch_core_free_flush_events(client, event_count);
        return status;
    }

    honch_flush_context_t flush_context = {0};
    status = honch_flush_context_snapshot(client, &flush_context);
    if (status != HONCH_OK) {
        honch_core_free_flush_events(client, event_count);
        return status;
    }

    honch_payload_t compact_message = {0};
    size_t encoded_event_count = event_count;
    if (client->transport == NULL || client->transport->post_chunk == NULL) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    } else {
#ifdef HONCH_FLUSH_TIMING
        uint64_t encode_start_ms = honch_flush_timing_now_ms(client);
#endif
        status = honch_core_build_wire_v2_message(
            client,
            &flush_context,
            events,
            event_count,
            &compact_message,
            &encoded_event_count);
#ifdef HONCH_FLUSH_TIMING
        uint64_t encode_elapsed_ms = honch_flush_timing_now_ms(client) - encode_start_ms;
        snprintf(
            timing_message,
            sizeof(timing_message),
            "HONCH_FLUSH_TIMING phase=encode input_events=%u encoded_events=%u message_bytes=%u status=%d elapsed_ms=%llu",
            (unsigned)event_count,
            (unsigned)encoded_event_count,
            (unsigned)compact_message.length,
            (int)status,
            (unsigned long long)encode_elapsed_ms);
        honch_flush_timing_log(client, timing_message);
#endif
    }
    honch_core_free_flush_events(client, event_count);
    if (status != HONCH_OK) {
        honch_flush_context_free(&flush_context);
        if (status == HONCH_ERROR_INVALID_ARGUMENT && encoded_event_count < event_count) {
            honch_status_t dead_status = honch_core_queue_dead_letter(client, sequences[encoded_event_count]);
            if (dead_status == HONCH_OK) {
                *progressed = true;
                return HONCH_ERROR_REJECTED;
            }
            return dead_status;
        }
        return status;
    }
    event_count = encoded_event_count;

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    uint32_t message_id = flush_context.message_id_seed + (uint32_t)sequences[0];
    client->pending_flush_message_size = compact_message.length;
    client->pending_flush_message_offset = 0u;
    client->pending_flush_event_count = event_count;
    client->pending_flush_message_id = message_id;
    snprintf(
        client->pending_flush_stream_id,
        sizeof(client->pending_flush_stream_id),
        "%s",
        flush_context.stream_id);
    client->pending_flush_active = true;
    honch_client_state_unlock(client);
#ifdef HONCH_FLUSH_TIMING
    uint64_t post_message_start_ms = honch_flush_timing_now_ms(client);
#endif
    bool message_complete = false;
    status = honch_core_post_wire_v2_message_limited(
        client,
        &compact_message,
        message_id,
        flush_context.stream_id,
        max_chunks,
        &client->pending_flush_message_offset,
        &message_complete,
        &result);
    client->outbound_upload_attempted = true;
#ifdef HONCH_FLUSH_TIMING
    uint64_t post_message_elapsed_ms = honch_flush_timing_now_ms(client) - post_message_start_ms;
    snprintf(
        timing_message,
        sizeof(timing_message),
        "HONCH_FLUSH_TIMING phase=post_message events=%u message_bytes=%u status=%d result=%d elapsed_ms=%llu",
        (unsigned)event_count,
        (unsigned)compact_message.length,
        (int)status,
        (int)result,
        (unsigned long long)post_message_elapsed_ms);
    honch_flush_timing_log(client, timing_message);
#endif
    honch_status_t relock_status = honch_client_state_lock(client);
    honch_flush_context_free(&flush_context);
    if (relock_status != HONCH_OK) {
        return relock_status;
    }
    if (status == HONCH_OK && !message_complete) {
        client->scheduler_flush_requested = true;
        return HONCH_OK;
    }

    if (result == HONCH_TRANSPORT_ACCEPTED && status == HONCH_OK) {
#ifdef HONCH_FLUSH_TIMING
        uint64_t consume_start_ms = honch_flush_timing_now_ms(client);
#endif
        status = honch_core_queue_consume_batch(client, sequences, event_count);
#ifdef HONCH_FLUSH_TIMING
        uint64_t consume_elapsed_ms = honch_flush_timing_now_ms(client) - consume_start_ms;
        snprintf(
            timing_message,
            sizeof(timing_message),
            "HONCH_FLUSH_TIMING phase=consume_batch events=%u status=%d elapsed_ms=%llu",
            (unsigned)event_count,
            (int)status,
            (unsigned long long)consume_elapsed_ms);
        honch_flush_timing_log(client, timing_message);
#endif
        *progressed = status == HONCH_OK;
        if (status == HONCH_OK) {
            honch_pending_flush_clear(client);
        }
        return status;
    }

    if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
        honch_status_t dead_status = honch_core_queue_dead_letter_batch(client, sequences, event_count);
        if (dead_status == HONCH_OK) {
            honch_pending_flush_clear(client);
            *progressed = true;
            return status == HONCH_OK ? HONCH_ERROR_REJECTED : status;
        }
        return dead_status;
    }

    return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
}

honch_status_t honch_queue_flush_one_locked(honch_client_t *client, bool *progressed)
{
    return honch_queue_flush_one_with_chunk_limit_locked(client, SIZE_MAX, progressed);
}

honch_status_t honch_queue_flush_one_chunk_locked(honch_client_t *client, bool *progressed)
{
    return honch_queue_flush_one_with_chunk_limit_locked(client, HONCH_TICK_MAX_CHUNKS, progressed);
}

honch_status_t honch_queue_flush_limited_locked(honch_client_t *client, size_t max_batches)
{
    honch_status_t final_status = HONCH_OK;
    size_t batches_flushed = 0u;

    for (;;) {
        size_t depth = 0u;
#ifdef HONCH_FLUSH_TIMING
        uint64_t depth_start_ms = honch_flush_timing_now_ms(client);
#endif
        honch_status_t status = honch_core_queue_depth(client, &depth);
#ifdef HONCH_FLUSH_TIMING
        uint64_t depth_elapsed_ms = honch_flush_timing_now_ms(client) - depth_start_ms;
        char timing_message[160];
        snprintf(
            timing_message,
            sizeof(timing_message),
            "HONCH_FLUSH_TIMING phase=queue_depth depth=%u status=%d elapsed_ms=%llu",
            (unsigned)depth,
            (int)status,
            (unsigned long long)depth_elapsed_ms);
        honch_flush_timing_log(client, timing_message);
#endif
        if (status != HONCH_OK) {
            return status;
        }
        client->queued_event_count = depth;
        if (depth == 0u) {
            return final_status;
        }
        if (batches_flushed >= max_batches) {
            return final_status;
        }

        bool progressed = false;
        status = honch_queue_flush_one_locked(client, &progressed);
        batches_flushed++;
        if (status == HONCH_ERROR_REJECTED && progressed) {
            final_status = HONCH_ERROR_REJECTED;
            continue;
        }
        if (status != HONCH_OK) {
            return status;
        }
    }
}

honch_status_t honch_queue_flush_locked(honch_client_t *client)
{
    return honch_queue_flush_limited_locked(client, SIZE_MAX);
}
