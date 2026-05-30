// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#define HONCH_CORE_NO_SHORT_STATUS_NAMES

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"
#include "honch/core/platform.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

#include "freertos/FreeRTOS.h"

#define HONCH_ESP_RAM_QUEUE_MAX_EVENTS 256u

typedef honch_client_t honch_esp_core_client_t;

typedef struct honch_esp_platform {
    void *reserved;
} honch_esp_platform_t;

typedef struct honch_esp_ram_queue_entry {
    uint64_t sequence;
    uint32_t offset;
    uint32_t size;
} honch_esp_ram_queue_entry_t;

typedef struct honch_esp_storage {
    uint64_t peek_sequence;
    uint64_t read_sequence;
    honch_esp_ram_queue_entry_t *ram_entries;
    uint8_t *ram_buffer;
    size_t ram_buffer_size;
    size_t ram_used;
    size_t ram_count;
    size_t ram_entry_capacity;
    bool nvs_fallback_active;
} honch_esp_storage_t;

typedef struct honch_esp_transport {
    void *reserved;
} honch_esp_transport_t;

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx);
void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx);
honch_status_t honch_esp_storage_ops_init(honch_storage_ops_t *ops, honch_esp_storage_t *ctx);
honch_status_t honch_esp_storage_use_ram_queue(
    honch_esp_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size);
honch_status_t honch_esp_transport_ops_init(honch_transport_ops_t *ops, honch_esp_transport_t *ctx);
