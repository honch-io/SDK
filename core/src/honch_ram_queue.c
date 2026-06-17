#include "honch/core/ram_queue.h"

#include <stdlib.h>
#include <string.h>

static void honch_ram_queue_refresh_stats(honch_ram_queue_t *queue)
{
    queue->stats.queued_events = queue->count;
    queue->stats.queued_bytes = queue->used_bytes;
    queue->stats.buffer_bytes = queue->buffer_size;
    if (queue->count > queue->stats.high_water_events) {
        queue->stats.high_water_events = queue->count;
    }
    if (queue->used_bytes > queue->stats.high_water_bytes) {
        queue->stats.high_water_bytes = queue->used_bytes;
    }
}

static honch_status_t honch_ram_queue_remove_at(honch_ram_queue_t *queue, size_t index)
{
    if (queue == NULL || index >= queue->count) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t offset = queue->entries[index].offset;
    size_t length = queue->entries[index].length;
    if (length > queue->used_bytes || offset > queue->used_bytes - length) {
        return HONCH_ERROR_INTERNAL;
    }

    size_t tail_offset = offset + length;
    size_t tail_bytes = queue->used_bytes - tail_offset;
    if (tail_bytes > 0u) {
        memmove(queue->buffer + offset, queue->buffer + tail_offset, tail_bytes);
    }

    for (size_t i = 0u; i < queue->count; i++) {
        if (queue->entries[i].offset > offset) {
            queue->entries[i].offset -= length;
        }
    }
    if (index + 1u < queue->count) {
        memmove(
            &queue->entries[index],
            &queue->entries[index + 1u],
            (queue->count - index - 1u) * sizeof(queue->entries[0]));
    }
    queue->count--;
    queue->used_bytes -= length;
    if (queue->read_index > index) {
        queue->read_index--;
    } else if (queue->read_index > queue->count) {
        queue->read_index = queue->count;
    }
    honch_ram_queue_refresh_stats(queue);
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || event == NULL || event_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (event_size > queue->buffer_size || queue->entry_capacity == 0u) {
        queue->stats.oversized_rejected_events++;
        return HONCH_ERROR_QUEUE_FULL;
    }

    while ((queue->count >= queue->entry_capacity || event_size > queue->buffer_size - queue->used_bytes) &&
           queue->count > 0u) {
        honch_status_t status = honch_ram_queue_remove_at(queue, 0u);
        if (status != HONCH_OK) {
            queue->stats.enqueue_failures++;
            return status;
        }
        queue->stats.capacity_dropped_events++;
    }

    if (queue->count >= queue->entry_capacity || event_size > queue->buffer_size - queue->used_bytes) {
        queue->stats.enqueue_failures++;
        return HONCH_ERROR_QUEUE_FULL;
    }

    memcpy(queue->buffer + queue->used_bytes, event, event_size);
    queue->entries[queue->count++] = (honch_ram_queue_entry_t) {
        .offset = queue->used_bytes,
        .length = event_size,
        .sequence = sequence
    };
    queue->used_bytes += event_size;
    honch_ram_queue_refresh_stats(queue);
    return HONCH_OK;
}

static honch_status_t honch_ram_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || buffer == NULL || queue->read_index >= queue->count) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_ram_queue_entry_t *entry = &queue->entries[queue->read_index];
    if ((size_t)offset > entry->length || buffer_size > entry->length - (size_t)offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memcpy(buffer, queue->buffer + entry->offset + offset, buffer_size);
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || reader == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (queue->count == 0u) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }
    if (queue->read_index >= queue->count) {
        queue->read_index = 0u;
    }
    honch_ram_queue_entry_t *entry = &queue->entries[queue->read_index];
    *reader = (honch_storage_reader_t) {
        .ctx = queue,
        .read = honch_ram_reader_read,
        .total_size = entry->length,
        .sequence = entry->sequence
    };
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_read_batch(
    void *ctx,
    honch_storage_event_t *events,
    size_t max_events,
    size_t max_event_bytes,
    size_t *event_count)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || events == NULL || event_count == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *event_count = 0u;
    size_t bytes = 0u;
    for (size_t i = 0u; i < queue->count && *event_count < max_events; i++) {
        honch_ram_queue_entry_t *entry = &queue->entries[i];
        if (*event_count > 0u && entry->length > max_event_bytes - bytes) {
            break;
        }
        if (entry->length > max_event_bytes) {
            return HONCH_ERROR_QUEUE_FULL;
        }
        events[*event_count] = (honch_storage_event_t) {
            .data = queue->buffer + entry->offset,
            .length = entry->length,
            .sequence = entry->sequence,
            .flags = HONCH_STORAGE_EVENT_BORROWED
        };
        bytes += entry->length;
        (*event_count)++;
    }
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_consume(void *ctx, uint64_t sequence)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < queue->count; i++) {
        if (queue->entries[i].sequence == sequence) {
            return honch_ram_queue_remove_at(queue, i);
        }
    }
    return HONCH_ERROR_NOT_INITIALIZED;
}

static honch_status_t honch_ram_queue_consume_batch(void *ctx, const uint64_t *sequences, size_t sequence_count)
{
    if (sequences == NULL && sequence_count > 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        status = honch_ram_queue_consume(ctx, sequences[i]);
    }
    return status;
}

static honch_status_t honch_ram_queue_dead_letter(void *ctx, uint64_t sequence)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    honch_status_t status = honch_ram_queue_consume(ctx, sequence);
    if (status == HONCH_OK) {
        queue->stats.dead_lettered_events++;
    }
    return status;
}

static honch_status_t honch_ram_queue_dead_letter_batch(void *ctx, const uint64_t *sequences, size_t sequence_count)
{
    if (sequences == NULL && sequence_count > 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        status = honch_ram_queue_dead_letter(ctx, sequences[i]);
    }
    return status;
}

static honch_status_t honch_ram_queue_drop_oldest(void *ctx)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (queue->count == 0u) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }
    honch_status_t status = honch_ram_queue_remove_at(queue, 0u);
    if (status == HONCH_OK) {
        queue->stats.capacity_dropped_events++;
    }
    return status;
}

static honch_status_t honch_ram_queue_clear(void *ctx)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    queue->used_bytes = 0u;
    queue->count = 0u;
    queue->read_index = 0u;
    honch_ram_queue_refresh_stats(queue);
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_depth(void *ctx, size_t *depth)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *depth = queue->count;
    return HONCH_OK;
}

static honch_status_t honch_ram_queue_get_stats(void *ctx, honch_queue_stats_t *stats)
{
    honch_ram_queue_t *queue = (honch_ram_queue_t *)ctx;
    if (queue == NULL || stats == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_ram_queue_refresh_stats(queue);
    *stats = queue->stats;
    return HONCH_OK;
}

honch_status_t honch_ram_queue_init_capped(
    honch_ram_queue_t *queue, uint8_t *buffer, size_t buffer_size, size_t max_events)
{
    if (queue == NULL || buffer == NULL || buffer_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memset(queue, 0, sizeof(*queue));
    queue->buffer = buffer;
    queue->buffer_size = buffer_size;
    /* Worst case is one entry per smallest (16-byte) event, but the client caps
     * the live queue at max_events, so metadata beyond that is never reachable.
     * Provision the smaller of the two so the entry array does not dwarf the
     * caller buffer it tracks. max_events == 0 keeps the legacy buffer-derived
     * bound for callers that do not plumb the cap. */
    queue->entry_capacity = buffer_size / 16u;
    if (max_events != 0u && max_events < queue->entry_capacity) {
        queue->entry_capacity = max_events;
    }
    if (queue->entry_capacity == 0u) {
        queue->entry_capacity = 1u;
    }
    queue->entries = (honch_ram_queue_entry_t *)calloc(queue->entry_capacity, sizeof(queue->entries[0]));
    if (queue->entries == NULL) {
        memset(queue, 0, sizeof(*queue));
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    honch_ram_queue_refresh_stats(queue);
    return HONCH_OK;
}

honch_status_t honch_ram_queue_init(honch_ram_queue_t *queue, uint8_t *buffer, size_t buffer_size)
{
    return honch_ram_queue_init_capped(queue, buffer, buffer_size, 0u);
}

void honch_ram_queue_deinit(honch_ram_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    free(queue->entries);
    memset(queue, 0, sizeof(*queue));
}

honch_status_t honch_ram_queue_ops_init(honch_event_queue_ops_t *ops, honch_ram_queue_t *queue)
{
    if (ops == NULL || queue == NULL || queue->buffer == NULL || queue->entries == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *ops = (honch_event_queue_ops_t) {
        .queue_push = honch_ram_queue_push,
        .queue_peek = honch_ram_queue_peek,
        .queue_read_batch = honch_ram_queue_read_batch,
        .queue_consume = honch_ram_queue_consume,
        .queue_consume_batch = honch_ram_queue_consume_batch,
        .queue_dead_letter = honch_ram_queue_dead_letter,
        .queue_dead_letter_batch = honch_ram_queue_dead_letter_batch,
        .queue_drop_oldest = honch_ram_queue_drop_oldest,
        .queue_clear = honch_ram_queue_clear,
        .queue_depth = honch_ram_queue_depth,
        .queue_get_stats = honch_ram_queue_get_stats,
        .ctx = queue
    };
    return HONCH_OK;
}
