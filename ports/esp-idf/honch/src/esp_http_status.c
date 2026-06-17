// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_http_status.h"

honch_status_t honch_esp_classify_http_response(bool transport_ok, int status,
                                                honch_transport_result_t *result)
{
    if (!transport_ok) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    if (status == 0) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    if (status == 202) {
        *result = HONCH_TRANSPORT_CHUNK_STORED;
        return HONCH_STATUS_OK;
    }
    if (status == 204) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_STATUS_OK;
    }
    if (status == 401) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
        return HONCH_STATUS_OK;
    }
    if (status == 429) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_RATE_LIMITED;
    }
    if (status == 408) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TIMEOUT;
    }
    if (status == 409) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    if (status >= 400 && status < 500) {
        *result = HONCH_TRANSPORT_REJECTED;
        return HONCH_STATUS_OK;
    }
    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_STATUS_ERROR_SERVER;
}
