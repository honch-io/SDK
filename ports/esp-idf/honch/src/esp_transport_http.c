// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch_internal.h"

#include "esp_core_adapter.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "honch";

static char *honch_esp_endpoint_url(const char *endpoint_url, const char *suffix)
{
    size_t endpoint_length = strlen(endpoint_url);
    while (endpoint_length > 0u && endpoint_url[endpoint_length - 1u] == '/') {
        endpoint_length--;
    }
    size_t suffix_length = strlen(suffix);
    if (endpoint_length > SIZE_MAX - suffix_length - 1u) {
        return NULL;
    }

    char *url = (char *)malloc(endpoint_length + suffix_length + 1u);
    if (url == NULL) {
        return NULL;
    }

    memcpy(url, endpoint_url, endpoint_length);
    memcpy(url + endpoint_length, suffix, suffix_length);
    url[endpoint_length + suffix_length] = '\0';
    return url;
}

static char *honch_esp_chunk_url(const char *endpoint_url)
{
    return honch_esp_endpoint_url(endpoint_url, "/capture");
}

static honch_status_t honch_esp_post_chunk(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    honch_client_t *core_client = (honch_client_t *)ctx;
    if (core_client == NULL || endpoint_url == NULL || api_key == NULL || body == NULL || body_size == 0u ||
        result == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (body_size > (size_t)INT_MAX) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *result = HONCH_TRANSPORT_RETRY;
    char *url = honch_esp_chunk_url(endpoint_url);
    if (url == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }

    int timeout_ms = core_client->transport_timeout_ms > (unsigned int)INT_MAX ?
        INT_MAX :
        (int)core_client->transport_timeout_ms;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms == 0 ? 10000 : timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        free(url);
        return HONCH_STATUS_ERROR_TRANSPORT;
    }

    esp_err_t err = esp_http_client_set_header(http_client, "Content-Type", "application/vnd.honch.chunk");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(http_client, "X-Honch-Project-Key", api_key);
    }
    if (err == ESP_OK && stream_id != NULL && stream_id[0] != '\0') {
        err = esp_http_client_set_header(http_client, "X-Honch-Stream-Id", stream_id);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(http_client, (const char *)body, (int)body_size);
    }
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "HONCH_HTTP_PAYLOAD format=chunk bytes=%u url=%s stream_id=%s",
            (unsigned)body_size,
            url,
            stream_id == NULL || stream_id[0] == '\0' ? "<none>" : stream_id);
        err = esp_http_client_perform(http_client);
    }

    int status = err == ESP_OK ? esp_http_client_get_status_code(http_client) : 0;
    esp_http_client_cleanup(http_client);
    free(url);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP chunk request failed: %s", esp_err_to_name(err));
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

honch_status_t honch_esp_transport_ops_init(honch_transport_ops_t *ops, honch_esp_transport_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_esp_transport_t) {0};
    *ops = (honch_transport_ops_t) {
        .post_chunk = honch_esp_post_chunk,
        .ctx = NULL
    };
    return HONCH_STATUS_OK;
}
