#include "honch/core/packetizer.h"
#include "honch_internal.h"

#include <string.h>

#define HONCH_PACKETIZER_VERSION 1u
#define HONCH_PACKETIZER_SOURCE_EVENTS 1u
#define HONCH_PACKETIZER_FLAG_FIRST 0x01u
#define HONCH_PACKETIZER_FLAG_FINAL 0x02u
#define HONCH_PACKETIZER_HEADER_SIZE 20u
#define HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD UINT16_MAX

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
    if (reader.read == NULL || reader.total_size == 0u || reader.total_size > UINT32_MAX) {
        honch_client_state_unlock(client);
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *packetizer = (honch_packetizer_t) {
        .client = client,
        .source_mask = source_mask,
        .sequence = reader.sequence,
        .offset = 0u,
        .total_size = reader.total_size,
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
    if (reader.read == NULL || reader.sequence != packetizer->sequence ||
        reader.total_size != packetizer->total_size) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t remaining = packetizer->total_size - packetizer->offset;
    size_t payload_capacity = buffer_size - HONCH_PACKETIZER_HEADER_SIZE;
    if (payload_capacity > HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD) {
        payload_capacity = HONCH_PACKETIZER_MAX_CHUNK_PAYLOAD;
    }
    size_t payload_size = remaining < payload_capacity ? remaining : payload_capacity;
    if (payload_size == 0u) {
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

    status = reader.read(reader.ctx, packetizer->offset, buffer + HONCH_PACKETIZER_HEADER_SIZE, payload_size);
    if (status != HONCH_OK) {
        return status;
    }

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

    honch_status_t status = packetizer->client->storage->queue_consume(
        packetizer->client->storage->ctx,
        packetizer->sequence);
    if (status == HONCH_OK) {
        packetizer->active = false;
        honch_client_state_unlock(packetizer->client);
        honch_client_leave(packetizer->client);
    }
    return status;
}

honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer)
{
    if (packetizer == NULL || !packetizer->active) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    packetizer->active = false;
    honch_client_state_unlock(packetizer->client);
    honch_client_leave(packetizer->client);
    return HONCH_OK;
}
