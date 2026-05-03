// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"

typedef enum {
    HONCH_TRANSPORT_OK = 0,
    HONCH_TRANSPORT_AUTH_ERROR,     // 401 - don't retry
    HONCH_TRANSPORT_SERVER_ERROR,   // 5xx - retry with backoff
    HONCH_TRANSPORT_NETWORK_ERROR,  // connection failed - retry with backoff
} honch_transport_result_t;

honch_err_t honch_transport_init(const char *host);
void honch_transport_deinit(void);

// Send a gzipped batch payload. body is raw JSON, will be gzipped internally.
honch_transport_result_t honch_transport_send(const char *body, size_t body_len);
