// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev
//
// Host unit test for the ESP-IDF Retry-After parser. The parser is pure (no
// ESP-IDF dependencies), so it is compiled and exercised directly on the host
// rather than only being grepped for in source.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_retry_after.h"

// A wall-clock value comfortably AFTER the dates used below (~year 2030).
#define NOW_2030_MS 1893456000000ULL
// A wall-clock value comfortably BEFORE the dates used below (~year 2000).
#define NOW_2000_MS 946684800000ULL
#define DATE_2026 "Wed, 21 Oct 2026 07:28:10 GMT"

static void test_delta_seconds(void)
{
    uint64_t d = 0;
    assert(honch_esp_parse_retry_after("120", 0, &d) && d == 120000u);
    assert(honch_esp_parse_retry_after("  30  ", NOW_2000_MS, &d) && d == 30000u);
    assert(honch_esp_parse_retry_after("0", 0, &d) && d == 0u);
    printf("  delta-seconds OK\n");
}

static void test_garbage_rejected(void)
{
    uint64_t d = 12345u;
    assert(!honch_esp_parse_retry_after("soon", 0, &d));
    assert(!honch_esp_parse_retry_after("", 0, &d));
    assert(!honch_esp_parse_retry_after("Wed, 21 Foo 2026 07:28:10 GMT", NOW_2000_MS, &d));
    assert(!honch_esp_parse_retry_after("Wed, 21 Oct 2026 07:28:10 PST", NOW_2000_MS, &d));
    printf("  garbage rejected OK\n");
}

static void test_http_date_with_clock(void)
{
    uint64_t d = 0;
    // Target is in the past relative to a 2030 clock -> no wait.
    assert(honch_esp_parse_retry_after(DATE_2026, NOW_2030_MS, &d) && d == 0u);
    // Target is in the future relative to a 2000 clock -> positive wait.
    assert(honch_esp_parse_retry_after(DATE_2026, NOW_2000_MS, &d) && d > 0u);
    printf("  http-date with clock OK\n");
}

// H8: a valid HTTP-date Retry-After received before the clock is set (now_ms==0)
// must still produce a backoff (the conservative fallback), not be ignored.
static void test_http_date_without_clock_uses_fallback(void)
{
    uint64_t d = 0;
    assert(honch_esp_parse_retry_after(DATE_2026, 0, &d));
    assert(d == HONCH_ESP_RETRY_AFTER_NO_CLOCK_FALLBACK_MS);
    printf("  http-date without clock falls back OK (delay=%llu ms)\n",
           (unsigned long long)d);
}

int main(void)
{
    test_delta_seconds();
    test_garbage_rejected();
    test_http_date_with_clock();
    test_http_date_without_clock_uses_fallback();
    printf("ALL RETRY-AFTER TESTS PASSED\n");
    return 0;
}
