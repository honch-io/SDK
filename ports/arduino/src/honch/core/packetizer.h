#ifndef HONCH_CORE_PACKETIZER_H
#define HONCH_CORE_PACKETIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"
#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_data_source_mask {
    HONCH_DATA_SOURCE_EVENTS = 1u << 0,
    HONCH_DATA_SOURCE_LOGS = 1u << 1,
    HONCH_DATA_SOURCE_DIAGNOSTICS = 1u << 2,
    HONCH_DATA_SOURCE_ALL = HONCH_DATA_SOURCE_EVENTS | HONCH_DATA_SOURCE_LOGS | HONCH_DATA_SOURCE_DIAGNOSTICS
} honch_data_source_mask_t;

typedef struct honch_packetizer {
    honch_client_t *client;
    uint32_t source_mask;
    uint64_t sequence;
    uint32_t offset;
    size_t total_size;
    bool active;
} honch_packetizer_t;

bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(
    honch_packetizer_t *packetizer,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size,
    bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);

#ifdef __cplusplus
}
#endif

#endif
