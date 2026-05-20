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

typedef struct honch_storage_ops {
    honch_status_t (*state_get)(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size);
    honch_status_t (*state_set)(void *ctx, const char *key, const uint8_t *data, size_t data_size);
    honch_status_t (*state_delete)(void *ctx, const char *key);
    honch_status_t (*queue_push)(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence);
    honch_status_t (*queue_peek)(void *ctx, honch_storage_reader_t *reader);
    honch_status_t (*queue_consume)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_dead_letter)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_drop_oldest)(void *ctx);
    honch_status_t (*queue_clear)(void *ctx);
    honch_status_t (*queue_depth)(void *ctx, size_t *depth);
    void *ctx;
} honch_storage_ops_t;

#ifdef __cplusplus
}
#endif

#endif
