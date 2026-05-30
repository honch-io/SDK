// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"

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
    HONCH_ERR_NVS,
    HONCH_ERR_TRANSPORT,
    HONCH_ERR_TIMEOUT,
    HONCH_ERR_BUSY,
    HONCH_ERR_INTERNAL,
} honch_err_t;

typedef enum {
    HONCH_GPIO_RISING_EDGE,
    HONCH_GPIO_FALLING_EDGE,
    HONCH_GPIO_BOTH_EDGES,
} honch_gpio_mode_t;

typedef struct {
    const char *api_key;                 // required
    const char *host;                    // required, e.g. "https://capture.honch.io"
    const char *device_model;            // required
    const char *firmware_version;        // required
    const char *environment;             // optional, defaults to "production"
    uint8_t *event_buffer;               // required, caller-owned, used for queue
    size_t event_buffer_size;            // required, recommend >= 8192
    uint32_t flush_interval_seconds;     // optional, default 60
    uint32_t flush_event_threshold;      // optional, default 30
    int (*battery_callback)(void);       // optional, returns 0-100 or -1 if unknown
    int battery_low_threshold;           // optional, default 15
    honch_durability_mode_t durability_mode; // optional, HONCH_DURABILITY_OS_BUFFERED default;
                                             // HONCH_DURABILITY_SYNC_ALWAYS for stronger queue persistence
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

honch_err_t honch_track_gpio(gpio_num_t pin, const char *event_name, honch_gpio_mode_t mode);

#ifdef __cplusplus
}
#endif
