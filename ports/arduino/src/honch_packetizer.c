#include "honch/core/packetizer.h"
#include "honch_internal.h"

#include <stdlib.h>
#include <string.h>

#define HONCH_PACKETIZER_VERSION 1u
#define HONCH_PACKETIZER_SOURCE_EVENTS 1u
#define HONCH_PACKETIZER_FLAG_FIRST 0x01u
#define HONCH_PACKETIZER_FLAG_FINAL 0x02u
#define HONCH_PACKETIZER_HEADER_SIZE 20u
#define HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD UINT16_MAX
#define HONCH_PACKETIZER_MAX_MESSAGE_BYTES 4096u
#define HONCH_MIN_UNIX_TIME_MS 1577836800000ULL

static void honch_packetizer_write_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void honch_packetizer_write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void honch_packetizer_write_u64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0u; i < 8u; i++) {
        out[7u - i] = (uint8_t)(value >> (i * 8u));
    }
}

static uint16_t honch_packetizer_crc16_update(uint16_t crc, const uint8_t *data, size_t data_size)
{
    for (size_t i = 0u; i < data_size; i++) {
        crc ^= (uint16_t)data[i] << 8u;
        for (size_t bit = 0u; bit < 8u; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }
    return crc;
}

static uint16_t honch_packetizer_frame_crc16(const uint8_t *header, const uint8_t *payload, size_t payload_size)
{
    uint16_t crc = honch_packetizer_crc16_update(0xffffu, header, 18u);
    return honch_packetizer_crc16_update(crc, payload, payload_size);
}

static bool honch_packetizer_source_supported(uint32_t source_mask)
{
    return (source_mask & HONCH_DATA_SOURCE_EVENTS) != 0u;
}

static uint64_t honch_packetizer_normalize_timestamp(honch_client_t *client, uint64_t timestamp_ms)
{
    if (client == NULL || client->platform == NULL ||
        client->platform->now_ms == NULL || client->platform->uptime_ms == NULL ||
        timestamp_ms == 0u || timestamp_ms >= HONCH_MIN_UNIX_TIME_MS) {
        return timestamp_ms;
    }

    uint64_t now_ms = client->platform->now_ms(client->platform->ctx);
    uint64_t uptime_ms = client->platform->uptime_ms(client->platform->ctx);
    if (now_ms < HONCH_MIN_UNIX_TIME_MS || uptime_ms == 0u ||
        now_ms <= uptime_ms || timestamp_ms > uptime_ms) {
        return timestamp_ms;
    }

    uint64_t boot_epoch_ms = now_ms - uptime_ms;
    if (UINT64_MAX - boot_epoch_ms < timestamp_ms) {
        return timestamp_ms;
    }
    return boot_epoch_ms + timestamp_ms;
}

static honch_status_t honch_packetizer_peek(honch_client_t *client, honch_storage_reader_t *reader)
{
    if (client == NULL || reader == NULL || client->storage == NULL || client->storage->queue_peek == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_peek(client->storage->ctx, reader);
}

static honch_status_t honch_packetizer_reset_peek_cursor(honch_client_t *client)
{
    if (client == NULL || client->storage == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    client->active_storage_reader_sequence = UINT64_MAX;
    if (client->storage->queue_depth == NULL) {
        return HONCH_OK;
    }

    size_t depth = 0u;
    return client->storage->queue_depth(client->storage->ctx, &depth);
}

static honch_status_t honch_packetizer_read_event(
    const honch_storage_reader_t *reader,
    honch_payload_t *event)
{
    event->data = NULL;
    event->length = 0u;
    if (reader == NULL || reader->read == NULL || reader->total_size == 0u) {
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
    event->data = data;
    event->length = reader->total_size;
    return HONCH_OK;
}

static honch_status_t honch_packetizer_build_wire_v2_message(
    honch_client_t *client,
    const honch_storage_reader_t *reader,
    honch_payload_t *message)
{
    message->data = NULL;
    message->length = 0u;
    if (client == NULL || reader == NULL ||
        client->device_id == NULL || client->device_model == NULL ||
        client->firmware_version == NULL || client->sdk_platform == NULL ||
        client->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_payload_t event = {0};
    honch_status_t status = honch_packetizer_read_event(reader, &event);
    if (status != HONCH_OK) {
        return status;
    }

    honch_event_record_t *record = (honch_event_record_t *)calloc(1u, sizeof(*record));
    if (record == NULL) {
        free(event.data);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    status = honch_event_record_parse(event.data, event.length, record);
    if (status != HONCH_OK) {
        free(record);
        free(event.data);
        return status;
    }
    honch_event_record_prepare_wire_properties(record);
    record->timestamp_ms = honch_packetizer_normalize_timestamp(client, record->timestamp_ms);

    honch_wire_v2_event_t compact_event = {
        .event_name = record->event_name,
        .timestamp_ms = record->timestamp_ms,
        .properties = record->properties,
        .property_count = record->property_count
    };
    honch_wire_v2_batch_context_t context = {
        .distinct_id = record->distinct_id,
        .device_id = client->device_id,
        .device_model = client->device_model,
        .firmware_version = client->firmware_version,
        .sdk_platform = client->sdk_platform,
        .sdk_version = HONCH_SDK_VERSION,
        .environment = client->environment,
        .session_id = record->session_id != NULL ? record->session_id : client->session_id
    };

    uint8_t *buffer = (uint8_t *)malloc(HONCH_PACKETIZER_MAX_MESSAGE_BYTES);
    if (buffer == NULL) {
        honch_event_record_free(record);
        free(record);
        free(event.data);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    size_t message_size = 0u;
    status = honch_wire_v2_encode_event_batch(
        &context,
        record->timestamp_ms,
        &compact_event,
        1u,
        buffer,
        HONCH_PACKETIZER_MAX_MESSAGE_BYTES,
        &message_size);
    honch_event_record_free(record);
    free(record);
    free(event.data);
    if (status != HONCH_OK) {
        free(buffer);
        return status;
    }
    message->data = buffer;
    message->length = message_size;
    return HONCH_OK;
}

bool honch_core_data_available(honch_client_t *client, uint32_t source_mask)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return false;
    }

    if (!honch_packetizer_source_supported(source_mask) ||
        client->storage == NULL || client->storage->queue_depth == NULL) {
        honch_client_leave(client);
        return false;
    }

    size_t depth = 0u;
    bool available = client->storage->queue_depth(client->storage->ctx, &depth) == HONCH_OK && depth > 0u;
    honch_client_leave(client);
    return available;
}

honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    if (packetizer == NULL || !honch_packetizer_source_supported(source_mask)) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    status = honch_client_state_lock(client);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_packetizer_reset_peek_cursor(client);
    if (status != HONCH_OK) {
        honch_client_state_unlock(client);
        honch_client_leave(client);
        return status;
    }

    honch_storage_reader_t reader = {0};
    status = honch_packetizer_peek(client, &reader);
    if (status != HONCH_OK) {
        honch_client_state_unlock(client);
        honch_client_leave(client);
        return status;
    }
    if (reader.read == NULL || reader.total_size == 0u) {
        honch_client_state_unlock(client);
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_payload_t message = {0};
    status = honch_packetizer_build_wire_v2_message(client, &reader, &message);
    if (status != HONCH_OK || message.length == 0u || message.length > UINT32_MAX) {
        free(message.data);
        honch_client_state_unlock(client);
        honch_client_leave(client);
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }
    size_t total_size = message.length;
    free(message.data);

    *packetizer = (honch_packetizer_t) {
        .client = client,
        .source_mask = source_mask,
        .sequence = reader.sequence,
        .offset = 0u,
        .total_size = total_size,
        .active = true
    };
    return HONCH_OK;
}

honch_status_t honch_packetizer_next(
    honch_packetizer_t *packetizer,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size,
    bool *message_complete)
{
    if (packetizer == NULL || buffer == NULL || out_size == NULL || message_complete == NULL ||
        !packetizer->active || packetizer->client == NULL || buffer_size <= HONCH_PACKETIZER_HEADER_SIZE ||
        packetizer->offset >= packetizer->total_size) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = honch_packetizer_reset_peek_cursor(packetizer->client);
    if (status != HONCH_OK) {
        return status;
    }

    honch_storage_reader_t reader = {0};
    status = honch_packetizer_peek(packetizer->client, &reader);
    if (status != HONCH_OK) {
        return status;
    }
    if (reader.read == NULL || reader.sequence != packetizer->sequence) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_payload_t message = {0};
    status = honch_packetizer_build_wire_v2_message(packetizer->client, &reader, &message);
    if (status != HONCH_OK) {
        return status;
    }
    if (message.length != packetizer->total_size) {
        free(message.data);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t remaining = packetizer->total_size - packetizer->offset;
    size_t payload_capacity = buffer_size - HONCH_PACKETIZER_HEADER_SIZE;
    if (payload_capacity > HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD) {
        payload_capacity = HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD;
    }
    size_t payload_size = remaining < payload_capacity ? remaining : payload_capacity;
    if (payload_size == 0u) {
        free(message.data);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t flags = 0u;
    if (packetizer->offset == 0u) {
        flags |= HONCH_PACKETIZER_FLAG_FIRST;
    }
    if (payload_size == remaining) {
        flags |= HONCH_PACKETIZER_FLAG_FINAL;
    }

    buffer[0] = HONCH_PACKETIZER_VERSION;
    buffer[1] = HONCH_PACKETIZER_SOURCE_EVENTS;
    buffer[2] = flags;
    buffer[3] = 0u;
    honch_packetizer_write_u64(buffer + 4u, packetizer->sequence);
    honch_packetizer_write_u32(buffer + 12u, packetizer->offset);
    honch_packetizer_write_u16(buffer + 16u, (uint16_t)payload_size);
    buffer[18] = 0u;
    buffer[19] = 0u;

    memcpy(buffer + HONCH_PACKETIZER_HEADER_SIZE, message.data + packetizer->offset, payload_size);
    free(message.data);

    uint16_t crc = honch_packetizer_frame_crc16(
        buffer,
        buffer + HONCH_PACKETIZER_HEADER_SIZE,
        payload_size);
    honch_packetizer_write_u16(buffer + 18u, crc);

    packetizer->offset += (uint32_t)payload_size;
    *out_size = HONCH_PACKETIZER_HEADER_SIZE + payload_size;
    *message_complete = (flags & HONCH_PACKETIZER_FLAG_FINAL) != 0u;
    return HONCH_OK;
}

honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer)
{
    if (packetizer == NULL || !packetizer->active || packetizer->client == NULL ||
        packetizer->offset < packetizer->total_size || packetizer->client->storage == NULL ||
        packetizer->client->storage->queue_consume == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_client_t *client = packetizer->client;
    honch_status_t status = packetizer->client->storage->queue_consume(
        packetizer->client->storage->ctx,
        packetizer->sequence);
    packetizer->active = false;
    packetizer->client = NULL;
    honch_client_state_unlock(client);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer)
{
    if (packetizer == NULL || !packetizer->active || packetizer->client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_client_t *client = packetizer->client;
    packetizer->active = false;
    packetizer->client = NULL;
    honch_client_state_unlock(client);
    honch_client_leave(client);
    return HONCH_OK;
}
