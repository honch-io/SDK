// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <string.h>

#include "honch/core/ram_queue.h"

honch_status_t honch_esp_event_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_esp_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size,
    size_t max_events)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_ram_queue_init_capped(&ctx->ram_queue, buffer, buffer_size, max_events);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    status = honch_ram_queue_ops_init(ops, &ctx->ram_queue);
    if (status != HONCH_STATUS_OK) {
        honch_ram_queue_deinit(&ctx->ram_queue);
    }
    return status;
}

void honch_esp_event_queue_ops_deinit(honch_esp_storage_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    honch_ram_queue_deinit(&ctx->ram_queue);
    memset(ctx, 0, sizeof(*ctx));
}
