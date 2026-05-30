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
#include "esp_timer.h"

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
#ifdef HONCH_FLUSH_TIMING
    int64_t total_start_us = esp_timer_get_time();
#endif
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

#ifdef HONCH_FLUSH_TIMING
    int64_t init_start_us = esp_timer_get_time();
#endif
    esp_http_client_handle_t http_client = esp_http_client_init(&config);
#ifdef HONCH_FLUSH_TIMING
    int64_t init_us = esp_timer_get_time() - init_start_us;
#endif
    if (http_client == NULL) {
        free(url);
        return HONCH_STATUS_ERROR_TRANSPORT;
    }

#ifdef HONCH_FLUSH_TIMING
    int64_t setup_start_us = esp_timer_get_time();
#endif
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
#ifdef HONCH_FLUSH_TIMING
    int64_t setup_us = esp_timer_get_time() - setup_start_us;
    int64_t perform_us = 0;
#endif
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "HONCH_HTTP_PAYLOAD format=chunk bytes=%u url=%s stream_id=%s",
            (unsigned)body_size,
            url,
            stream_id == NULL || stream_id[0] == '\0' ? "<none>" : stream_id);
#ifdef HONCH_FLUSH_TIMING
        int64_t perform_start_us = esp_timer_get_time();
#endif
        err = esp_http_client_perform(http_client);
#ifdef HONCH_FLUSH_TIMING
        perform_us = esp_timer_get_time() - perform_start_us;
#endif
    }

    int status = err == ESP_OK ? esp_http_client_get_status_code(http_client) : 0;
#ifdef HONCH_FLUSH_TIMING
    int64_t cleanup_start_us = esp_timer_get_time();
#endif
    esp_http_client_cleanup(http_client);
#ifdef HONCH_FLUSH_TIMING
    int64_t cleanup_us = esp_timer_get_time() - cleanup_start_us;
#endif
    free(url);

    honch_status_t return_status = HONCH_STATUS_OK;
    if (err != ESP_OK) {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_TRANSPORT;
    } else if (status == 0) {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_TRANSPORT;
    } else if (status == 202) {
        *result = HONCH_TRANSPORT_CHUNK_STORED;
    } else if (status == 204) {
        *result = HONCH_TRANSPORT_ACCEPTED;
    } else if (status == 401) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
    } else if (status == 429) {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_RATE_LIMITED;
    } else if (status == 408) {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_TIMEOUT;
    } else if (status == 409) {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_TRANSPORT;
    } else if (status >= 400 && status < 500) {
        *result = HONCH_TRANSPORT_REJECTED;
    } else {
        *result = HONCH_TRANSPORT_RETRY;
        return_status = HONCH_STATUS_ERROR_SERVER;
    }

#ifdef HONCH_FLUSH_TIMING
    ESP_LOGI(
        TAG,
        "HONCH_HTTP_TIMING bytes=%u status=%d result=%d return_status=%d err=%s init_us=%lld setup_us=%lld perform_us=%lld cleanup_us=%lld total_us=%lld",
        (unsigned)body_size,
        status,
        (int)*result,
        (int)return_status,
        err == ESP_OK ? "ESP_OK" : esp_err_to_name(err),
        (long long)init_us,
        (long long)setup_us,
        (long long)perform_us,
        (long long)cleanup_us,
        (long long)(esp_timer_get_time() - total_start_us));
#endif

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP chunk request failed: %s", esp_err_to_name(err));
    }
    return return_status;
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
