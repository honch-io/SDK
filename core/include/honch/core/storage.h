#ifndef HONCH_CORE_STORAGE_H
#define HONCH_CORE_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_storage_reader {
    void *ctx;
    honch_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size);
    size_t total_size;
    uint64_t sequence;
} honch_storage_reader_t;

typedef struct honch_storage_event {
    uint8_t *data;
    size_t length;
    uint64_t sequence;
    uint8_t flags;
} honch_storage_event_t;

/* data points into queue-owned storage and must not be freed by the core. */
#define HONCH_STORAGE_EVENT_BORROWED 0x01u

typedef struct honch_queue_stats {
    size_t queued_events;
    size_t queued_bytes;
    size_t buffer_bytes;
    size_t high_water_events;
    size_t high_water_bytes;
    uint64_t capacity_dropped_events;
    uint64_t oversized_rejected_events;
    uint64_t enqueue_failures;
    uint64_t dead_lettered_events;
} honch_queue_stats_t;

typedef struct honch_state_storage_ops {
    honch_status_t (*state_get)(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size);
    honch_status_t (*state_set)(void *ctx, const char *key, const uint8_t *data, size_t data_size);
    honch_status_t (*state_delete)(void *ctx, const char *key);
    void *ctx;
} honch_state_storage_ops_t;

typedef struct honch_event_queue_ops {
    honch_status_t (*queue_push)(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence);
    honch_status_t (*queue_peek)(void *ctx, honch_storage_reader_t *reader);
    honch_status_t (*queue_read_batch)(
        void *ctx,
        honch_storage_event_t *events,
        size_t max_events,
        size_t max_event_bytes,
        size_t *event_count);
    honch_status_t (*queue_consume)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_consume_batch)(void *ctx, const uint64_t *sequences, size_t sequence_count);
    honch_status_t (*queue_dead_letter)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_dead_letter_batch)(void *ctx, const uint64_t *sequences, size_t sequence_count);
    honch_status_t (*queue_drop_oldest)(void *ctx);
    honch_status_t (*queue_clear)(void *ctx);
    honch_status_t (*queue_depth)(void *ctx, size_t *depth);
    honch_status_t (*queue_get_stats)(void *ctx, honch_queue_stats_t *stats);
    void *ctx;
} honch_event_queue_ops_t;

#ifdef __cplusplus
}
#endif

#endif
