// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HONCH_CORE_NO_SHORT_STATUS_NAMES
#define HONCH_CORE_NO_SHORT_STATUS_NAMES
#define HONCH_ESP_UNDEFINE_CORE_NO_SHORT_STATUS_NAMES
#endif
#include "honch/core/honch.h"
#ifdef HONCH_ESP_UNDEFINE_CORE_NO_SHORT_STATUS_NAMES
#undef HONCH_CORE_NO_SHORT_STATUS_NAMES
#undef HONCH_ESP_UNDEFINE_CORE_NO_SHORT_STATUS_NAMES
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HONCH_OK = 0,
    HONCH_ERR_INVALID_ARG,
    HONCH_ERR_NOT_INITIALIZED,
    HONCH_ERR_ALREADY_INITIALIZED,
    HONCH_ERR_NO_MEM,
    HONCH_ERR_QUEUE_FULL,
    HONCH_ERR_IO,
    HONCH_ERR_TRANSPORT,
    HONCH_ERR_TIMEOUT,
    HONCH_ERR_BUSY,
    HONCH_ERR_NOT_SUPPORTED,
    HONCH_ERR_OFFLINE,
    HONCH_ERR_INTERNAL,
} honch_err_t;

#ifndef HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS
#define HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS 0xffffffffu
#endif

#define HONCH_ESP_API_KEY_MAX_LENGTH 127u
#define HONCH_ESP_ENDPOINT_URL_MAX_LENGTH 279u
#define HONCH_ESP_DEVICE_MODEL_MAX_LENGTH 63u
#define HONCH_ESP_FIRMWARE_VERSION_MAX_LENGTH 31u
#define HONCH_ESP_ENVIRONMENT_MAX_LENGTH 31u

typedef struct {
    const char *api_key;                 // required
    const char *host;                    // optional, defaults to "https://i.honch.io"
    const char *device_model;            // required
    const char *firmware_version;        // required
    const char *environment;             // optional, defaults to "production"
    uint8_t *event_buffer;               // required, caller-owned, used for queue
    size_t event_buffer_size;            // required, recommend >= 8192
    uint32_t flush_interval_seconds;     // optional, default 120
    uint32_t flush_min_interval_ms;      // optional, default 15000
    uint32_t flush_event_threshold;      // optional, default 20
    uint32_t flush_max_batches;          // optional, default 1
    uint32_t shutdown_flush_max_batches; // optional, default 1
    uint32_t transport_timeout_ms;       // optional, default 8000
    int (*battery_callback)(void);       // optional, returns 0-100 or -1 if unknown
    int battery_low_threshold;           // optional, default 15
    bool (*connectivity_callback)(void); // optional, return false while offline/radio off
    const honch_state_storage_ops_t *state_storage_ops; // optional, enables durable identity/version state
    const honch_event_queue_ops_t *event_queue_ops;     // optional, replaces default RAM event queue
    bool enable_error_tracking;          // optional, emits the recovered $crash after an abnormal reset
    bool enable_crash_symbolication;     // optional, adds build ID and raw crash addresses when ESP-IDF coredump summary is available
} honch_config_t;

honch_err_t honch_init(const honch_config_t *config);
honch_err_t honch_shutdown(void);

honch_err_t honch_track(const char *event, const honch_property_t *properties, size_t property_count);
honch_err_t honch_identify(const char *distinct_id, const honch_property_t *properties, size_t property_count);
honch_err_t honch_set_property(const char *key, honch_value_t value);

honch_err_t honch_session_start(const char *session_name);
honch_err_t honch_session_end(void);

honch_err_t honch_flush(void);
honch_err_t honch_tick(void);
honch_err_t honch_reset(void);

const char *honch_get_device_id(void);
honch_err_t honch_get_queue_stats(honch_queue_stats_t *stats);

#ifdef __cplusplus
}
#endif
