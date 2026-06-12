// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#define HONCH_CORE_NO_SHORT_STATUS_NAMES

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"
#include "honch/core/platform.h"
#include "honch/core/ram_queue.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"

typedef honch_client_t honch_esp_core_client_t;

typedef struct honch_esp_platform {
    void *reserved;
} honch_esp_platform_t;

typedef struct honch_esp_storage {
    honch_ram_queue_t ram_queue;
} honch_esp_storage_t;

typedef struct honch_esp_transport {
    esp_http_client_handle_t http_client;
    char *endpoint_url;
    char *capture_url;
    int timeout_ms;
    int configured_timeout_ms;
    uint64_t retry_after_ms;
} honch_esp_transport_t;

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx);
void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx);
honch_status_t honch_esp_default_device_id(char *buffer, size_t buffer_size);
honch_fault_snapshot_t honch_esp_fault_snapshot(void);
honch_status_t honch_esp_event_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_esp_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size);
void honch_esp_event_queue_ops_deinit(honch_esp_storage_t *ctx);
honch_status_t honch_esp_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_esp_transport_t *ctx,
    unsigned int transport_timeout_ms);
void honch_esp_transport_ops_deinit(honch_esp_transport_t *ctx);
