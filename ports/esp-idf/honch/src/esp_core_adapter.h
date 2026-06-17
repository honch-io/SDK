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

#ifndef HONCH_ENABLE_CRASH_SYMBOLICATION
#define HONCH_ENABLE_CRASH_SYMBOLICATION 1
#endif

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

#define HONCH_ESP_BUILD_ID_MAX_BYTES 64u
#define HONCH_ESP_FAULT_PC_MAX_BYTES 18u
#define HONCH_ESP_BACKTRACE_MAX_BYTES 192u
#define HONCH_ESP_TASK_NAME_MAX_BYTES 32u

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx);
void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx);
honch_status_t honch_esp_default_device_id(char *buffer, size_t buffer_size);
#if HONCH_ENABLE_ERROR_TRACKING
honch_fault_snapshot_t honch_esp_fault_snapshot(bool include_symbolication_context);
#endif
honch_status_t honch_esp_event_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_esp_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size,
    size_t max_events);
void honch_esp_event_queue_ops_deinit(honch_esp_storage_t *ctx);
honch_status_t honch_esp_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_esp_transport_t *ctx,
    unsigned int transport_timeout_ms);
void honch_esp_transport_ops_deinit(honch_esp_transport_t *ctx);
