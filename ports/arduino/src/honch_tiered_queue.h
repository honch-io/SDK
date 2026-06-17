#ifndef HONCH_TIERED_QUEUE_H
#define HONCH_TIERED_QUEUE_H

/*
 * Two-tier event queue: a hot RAM tier (honch_ram_queue) plus an optional cold
 * non-volatile tier (honch_nv_queue_ops). Implements the standard
 * honch_event_queue_ops_t so the core drives it like any other queue.
 *
 * Events are written to RAM first. The tiered queue spills oldest RAM events to
 * the NV tier under memory pressure (configurable high/low water on RAM bytes)
 * or on an explicit honch_tiered_queue_persist() call (e.g. graceful shutdown).
 * Reads drain the NV tier (oldest, by sequence) ahead of RAM so ordering
 * survives reboots. NV records are framed and CRC-checked by this module:
 *
 *     [ seq:u64 LE ][ payload_len:u32 LE ][ payload ][ crc16 LE ]
 *
 * with crc16 (CCITT-FALSE) over seq+len+payload. A corrupt record (e.g. a torn
 * write at power loss) is detected on read and dropped without disturbing the
 * rest of the queue.
 */

#include "honch/core/storage.h"
#include "honch_nv_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_tiered_queue_config {
    /* Spill oldest RAM events to NV when RAM queued_bytes >= high_water, until
     * RAM queued_bytes <= low_water. high_water == 0 disables auto-spill. */
    size_t spill_high_water_bytes;
    size_t spill_low_water_bytes;
} honch_tiered_queue_config_t;

typedef struct honch_tiered_queue {
    honch_event_queue_ops_t ram_ops;    /* tier 1 (by value) */
    const honch_nv_queue_ops_t *nv_ops; /* tier 2, may be NULL */
    honch_tiered_queue_config_t config;
    uint8_t *read_scratch;              /* staging for NV read-back (BORROWED data) */
    size_t read_scratch_size;
    size_t peek_cursor;                 /* NV iteration cursor for startup recovery */
} honch_tiered_queue_t;

/* Initialise over an existing RAM queue's ops.
 *   nv_ops    may be NULL (RAM-only; config/scratch then ignored).
 *   config    may be NULL (no auto-spill).
 *   read_scratch must be large enough to hold at least one decoded NV event;
 *              larger scratch lets more NV events drain per batch. */
honch_status_t honch_tiered_queue_init(
    honch_tiered_queue_t *tq,
    const honch_event_queue_ops_t *ram_ops,
    const honch_nv_queue_ops_t *nv_ops,
    const honch_tiered_queue_config_t *config,
    uint8_t *read_scratch,
    size_t read_scratch_size);

/* Fill `ops` with delegating callbacks bound to `tq` (ops.ctx = tq). */
honch_status_t honch_tiered_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_tiered_queue_t *tq);

/* Spill oldest RAM events into NV until RAM queued_bytes <= target_bytes (or RAM
 * is empty / NV unavailable). target_bytes == 0 drains all of RAM to NV. Used
 * for graceful-shutdown flush and internally for memory-pressure spill. */
honch_status_t honch_tiered_queue_persist(honch_tiered_queue_t *tq, size_t target_bytes);

#ifdef __cplusplus
}
#endif

#endif
