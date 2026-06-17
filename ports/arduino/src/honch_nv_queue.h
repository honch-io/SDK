#ifndef HONCH_NV_QUEUE_H
#define HONCH_NV_QUEUE_H

/*
 * Storage-agnostic non-volatile event-queue interface.
 *
 * This is the second ("cold") tier behind honch_tiered_queue. The SDK frames,
 * checksums, and orders records; an implementation only has to store opaque
 * byte blobs durably as an oldest-first FIFO and expose them by logical index
 * (0 = oldest). No filesystem is assumed: an adapter can be backed by LittleFS,
 * SPIFFS, NVS, a raw flash partition, an SD card, or battery-backed RAM. A
 * consumer that supplies no adapter stays RAM-only with no behaviour change.
 */

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pulls the bytes of a blob being appended, so the tiered queue does not have
 * to stage a full copy. Reads [offset, offset + buffer_size) of the source. */
typedef honch_status_t (*honch_nv_read_cb)(
    void *cb_ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size);

typedef struct honch_nv_queue_stats {
    size_t stored_events;
    size_t stored_bytes;
    size_t capacity_bytes; /* 0 = unknown / unbounded */
    uint64_t dropped_events;
} honch_nv_queue_stats_t;

typedef struct honch_nv_queue_ops {
    /* Nonzero when persistence is usable right now (e.g. filesystem mounted).
     * When zero (or NULL ops), the tiered queue stays RAM-only, no error. */
    int (*enabled)(void *ctx);

    /* Number of stored blobs (FIFO depth). */
    honch_status_t (*count)(void *ctx, size_t *count_out);

    /* Byte length of the blob at logical index (0 = oldest). */
    honch_status_t (*length_at)(void *ctx, size_t index, size_t *length_out);

    /* Read [offset, offset + buffer_size) of the blob at logical index. */
    honch_status_t (*read_at)(void *ctx, size_t index, uint32_t offset,
                              uint8_t *buffer, size_t buffer_size);

    /* Append a blob of total_size bytes, pulled via reader/reader_ctx. On
     * capacity overflow the implementation drops oldest to make room. */
    honch_status_t (*append)(void *ctx, honch_nv_read_cb reader, void *reader_ctx, size_t total_size);

    /* Drop the oldest `n` blobs (called after upload is accepted, or to drop
     * a corrupt head record). */
    honch_status_t (*consume_front)(void *ctx, size_t n);

    /* Optional. May be NULL. */
    honch_status_t (*get_stats)(void *ctx, honch_nv_queue_stats_t *stats);

    void *ctx;
} honch_nv_queue_ops_t;

#ifdef __cplusplus
}
#endif

#endif
