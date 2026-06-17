// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev
#ifndef HONCH_ESP_RETRY_AFTER_H
#define HONCH_ESP_RETRY_AFTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure parsing of the HTTP Retry-After header (no ESP-IDF dependencies, so it is
 * unit-testable on the host). Supports both the delta-seconds and the HTTP-date
 * forms (RFC 7231).
 */

/*
 * Backoff applied when a syntactically valid HTTP-date Retry-After is received
 * but the wall clock is not yet set (now_ms == 0, e.g. before SNTP sync at
 * boot). The exact delay cannot be computed without a clock, but the server has
 * explicitly asked the client to back off, so we honor it with a conservative
 * fixed delay instead of ignoring the header (which would let the device retry
 * sooner than the server requested).
 */
#define HONCH_ESP_RETRY_AFTER_NO_CLOCK_FALLBACK_MS 60000u

/*
 * Parse a Retry-After header value. now_ms is the current wall-clock time in
 * milliseconds since the Unix epoch, or 0 if the clock is not set. On success,
 * writes the backoff in milliseconds to *delay_ms and returns true; returns
 * false for input that is not a valid Retry-After value.
 */
bool honch_esp_parse_retry_after(const char *value, uint64_t now_ms, uint64_t *delay_ms);

#ifdef __cplusplus
}
#endif

#endif
