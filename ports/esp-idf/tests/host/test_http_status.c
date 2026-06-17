// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev
//
// Host unit test for the ESP-IDF HTTP-response classifier. The mapping is pure
// (no ESP-IDF dependencies), so its retain/drop contract is executed on the host
// rather than only grepped for in source.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_http_status.h"

static void expect(bool transport_ok, int status,
                   honch_transport_result_t want_result, honch_status_t want_status)
{
    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    honch_status_t got = honch_esp_classify_http_response(transport_ok, status, &result);
    assert(result == want_result);
    assert(got == want_status);
}

int main(void)
{
    // Request did not complete (network error) -> retain + retry.
    expect(false, 0, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_TRANSPORT);
    expect(false, 204, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_TRANSPORT);  // ok wins to transport_ok

    // No status code received -> retain + retry.
    expect(true, 0, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_TRANSPORT);

    // Success codes.
    expect(true, 202, HONCH_TRANSPORT_CHUNK_STORED, HONCH_STATUS_OK);
    expect(true, 204, HONCH_TRANSPORT_ACCEPTED, HONCH_STATUS_OK);

    // Auth: drop, stop retrying until config changes.
    expect(true, 401, HONCH_TRANSPORT_AUTH_ERROR, HONCH_STATUS_OK);

    // Retainable failures.
    expect(true, 429, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_RATE_LIMITED);
    expect(true, 408, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_TIMEOUT);
    expect(true, 409, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_TRANSPORT);

    // Other 4xx -> permanent drop.
    expect(true, 400, HONCH_TRANSPORT_REJECTED, HONCH_STATUS_OK);
    expect(true, 404, HONCH_TRANSPORT_REJECTED, HONCH_STATUS_OK);
    expect(true, 413, HONCH_TRANSPORT_REJECTED, HONCH_STATUS_OK);
    expect(true, 422, HONCH_TRANSPORT_REJECTED, HONCH_STATUS_OK);

    // 5xx (and anything else) -> retain + retry.
    expect(true, 500, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_SERVER);
    expect(true, 503, HONCH_TRANSPORT_RETRY, HONCH_STATUS_ERROR_SERVER);

    printf("ALL HTTP STATUS TESTS PASSED\n");
    return 0;
}
