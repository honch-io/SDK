// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t len;
} honch_payload_t;

typedef struct {
    bool battery_known;
    int battery_level;
    bool wifi_rssi_known;
    int wifi_rssi;
    bool wifi_rssi_cache_hit;
} honch_event_runtime_t;

void honch_payload_free(honch_payload_t *payload);
void honch_event_runtime_collect(honch_event_runtime_t *runtime);

// Encode a single event into a CBOR payload. Caller must free out with honch_payload_free.
// Merges auto-stamped properties with user-supplied properties_json.
honch_err_t honch_encode_event(const char *event_name,
                               const char *distinct_id,
                               const char *properties_json,
                               const honch_event_runtime_t *runtime,
                               honch_payload_t *out);

// Build the batch wrapper CBOR. Caller must free out with honch_payload_free.
// events is an array of CBOR event payloads, count is the number.
honch_err_t honch_encode_batch(const char *api_key,
                               const honch_payload_t *events,
                               int count,
                               honch_payload_t *out);
