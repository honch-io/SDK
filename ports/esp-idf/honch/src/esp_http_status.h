// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev
#ifndef HONCH_ESP_HTTP_STATUS_H
#define HONCH_ESP_HTTP_STATUS_H

#include <stdbool.h>

#include "honch/core/status.h"
#include "honch/core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure classification of an HTTP chunk-upload response (no ESP-IDF
 * dependencies, so it is unit-testable on the host). Mirrors the retain/drop
 * contract: 202/204 succeed, 401 is an auth error, 408/409/429/5xx and a failed
 * request are retried (events stay queued), other 4xx are rejected (dropped).
 *
 *   transport_ok : false if the HTTP request did not complete (network error).
 *   status       : the HTTP status code (0 when none was received).
 *   *result      : set to the transport result for the queue policy.
 *   returns      : the honch_status_t the transport reports to the core.
 */
honch_status_t honch_esp_classify_http_response(bool transport_ok, int status,
                                                honch_transport_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
