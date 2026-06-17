#ifndef HONCH_CORE_RAM_QUEUE_H
#define HONCH_CORE_RAM_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_ram_queue_entry {
    size_t offset;
    size_t length;
    uint64_t sequence;
} honch_ram_queue_entry_t;

typedef struct honch_ram_queue {
    uint8_t *buffer;
    size_t buffer_size;
    size_t used_bytes;
    honch_ram_queue_entry_t *entries;
    size_t entry_capacity;
    size_t count;
    size_t read_index;
    honch_queue_stats_t stats;
} honch_ram_queue_t;

honch_status_t honch_ram_queue_init(
    honch_ram_queue_t *queue,
    uint8_t *buffer,
    size_t buffer_size);
/* Like honch_ram_queue_init but caps the entry-metadata array at max_events
 * (the client's queue depth limit), so a large caller buffer does not force an
 * equally large hidden metadata allocation. max_events == 0 keeps the legacy
 * buffer-derived capacity. */
honch_status_t honch_ram_queue_init_capped(
    honch_ram_queue_t *queue,
    uint8_t *buffer,
    size_t buffer_size,
    size_t max_events);
void honch_ram_queue_deinit(honch_ram_queue_t *queue);
honch_status_t honch_ram_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_ram_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
