// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "transport.h"
#include "perf.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "honch";

static char s_url[256];

#ifdef CONFIG_HONCH_ENABLE_GZIP
static bool honch_gzip_payload(
    const uint8_t *body,
    size_t body_len,
    uint8_t **out,
    size_t *out_len)
{
    static const uint8_t header[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
    };

    size_t deflated_size = 0;
    uint8_t *deflated = tdefl_compress_mem_to_heap(
        body,
        body_len,
        &deflated_size,
        TDEFL_DEFAULT_MAX_PROBES);
    if (!deflated) {
        ESP_LOGW(TAG, "Gzip deflate allocation failed; sending raw CBOR");
        return false;
    }

    if (deflated_size > SIZE_MAX - sizeof(header) - 8u) {
        free(deflated);
        return false;
    }

    size_t compressed_size = sizeof(header) + deflated_size + 8u;
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) {
        ESP_LOGW(TAG, "Gzip allocation failed; sending raw CBOR");
        free(deflated);
        return false;
    }

    memcpy(compressed, header, sizeof(header));
    memcpy(compressed + sizeof(header), deflated, deflated_size);
    free(deflated);

    uint32_t crc = mz_crc32(MZ_CRC32_INIT, body, body_len);
    uint8_t *trailer = compressed + sizeof(header) + deflated_size;
    trailer[0] = (uint8_t)(crc & 0xffu);
    trailer[1] = (uint8_t)((crc >> 8u) & 0xffu);
    trailer[2] = (uint8_t)((crc >> 16u) & 0xffu);
    trailer[3] = (uint8_t)((crc >> 24u) & 0xffu);
    trailer[4] = (uint8_t)(body_len & 0xffu);
    trailer[5] = (uint8_t)((body_len >> 8u) & 0xffu);
    trailer[6] = (uint8_t)((body_len >> 16u) & 0xffu);
    trailer[7] = (uint8_t)((body_len >> 24u) & 0xffu);

    if (compressed_size < body_len) {
        *out = compressed;
        *out_len = compressed_size;
        return true;
    }

    free(compressed);
    return false;
}
#endif

honch_err_t honch_transport_init(const char *host)
{
    if (!host) {
        return HONCH_ERR_INVALID_ARG;
    }

    snprintf(s_url, sizeof(s_url), "%s/batch", host);
    ESP_LOGI(TAG, "Transport initialized, endpoint: %s", s_url);
    return HONCH_OK;
}

void honch_transport_deinit(void)
{
    s_url[0] = '\0';
}

honch_transport_result_t honch_transport_send(const uint8_t *body, size_t body_len)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!body || body_len == 0) {
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

#ifdef CONFIG_HONCH_LOG_VERBOSE
    ESP_LOGI(TAG, "Sending CBOR batch: %u bytes", (unsigned)body_len);
#endif

    const uint8_t *post_body = body;
    size_t post_body_len = body_len;
    uint8_t *compressed = NULL;
    bool use_gzip = false;
    int64_t gzip_us = 0;

#ifdef CONFIG_HONCH_ENABLE_GZIP
    int64_t gzip_start_us = HONCH_PERF_NOW_US();
    if (body_len >= CONFIG_HONCH_GZIP_MIN_BYTES &&
        honch_gzip_payload(body, body_len, &compressed, &post_body_len)) {
        post_body = compressed;
        use_gzip = true;
#ifdef CONFIG_HONCH_LOG_VERBOSE
        ESP_LOGI(TAG, "Gzipped CBOR batch: %u -> %u bytes",
                 (unsigned)body_len, (unsigned)post_body_len);
#endif
    }
    gzip_us = HONCH_PERF_ELAPSED_US(gzip_start_us);
#endif

    esp_http_client_config_t http_config = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    int64_t init_start_us = HONCH_PERF_NOW_US();
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    int64_t init_us = HONCH_PERF_ELAPSED_US(init_start_us);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(compressed);
        HONCH_PERF_LOG("HONCH_PERF_TRANSPORT",
                       "result=init_error total_us=%" PRId64 " init_us=%" PRId64
                       " body_bytes=%u post_bytes=%u gzip=%d heap_before=%" PRIu32
                       " heap_after=%" PRIu32,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       init_us,
                       (unsigned)body_len,
                       (unsigned)post_body_len,
                       use_gzip ? 1 : 0,
                       heap_before,
                       HONCH_PERF_HEAP_FREE());
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    int64_t setup_start_us = HONCH_PERF_NOW_US();
    esp_http_client_set_header(client, "Content-Type", "application/cbor");
    if (use_gzip) {
        esp_http_client_set_header(client, "Content-Encoding", "gzip");
    }
    esp_http_client_set_post_field(client, (const char *)post_body, post_body_len);
    int64_t setup_us = HONCH_PERF_ELAPSED_US(setup_start_us);

    int64_t perform_start_us = HONCH_PERF_NOW_US();
    esp_err_t err = esp_http_client_perform(client);
    int64_t perform_us = HONCH_PERF_ELAPSED_US(perform_start_us);
    honch_transport_result_t result;
    int status = 0;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        result = HONCH_TRANSPORT_NETWORK_ERROR;
    } else {
        status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Batch sent successfully (HTTP %d)", status);
            result = HONCH_TRANSPORT_OK;
        } else if (status == 401) {
            ESP_LOGE(TAG, "Authentication failed (HTTP 401) - check API key");
            result = HONCH_TRANSPORT_AUTH_ERROR;
        } else if (status == 429) {
            ESP_LOGW(TAG, "Rate limited (HTTP 429)");
            result = HONCH_TRANSPORT_SERVER_ERROR;
        } else if (status >= 400 && status < 500) {
            ESP_LOGE(TAG, "Request rejected (HTTP %d), dropping batch", status);
            result = HONCH_TRANSPORT_AUTH_ERROR;
        } else {
            ESP_LOGE(TAG, "Server error (HTTP %d)", status);
            result = HONCH_TRANSPORT_SERVER_ERROR;
        }
    }

    int64_t cleanup_start_us = HONCH_PERF_NOW_US();
    esp_http_client_cleanup(client);
    int64_t cleanup_us = HONCH_PERF_ELAPSED_US(cleanup_start_us);
    free(compressed);

    HONCH_PERF_LOG("HONCH_PERF_TRANSPORT",
                   "result=%d esp_err=%d status=%d total_us=%" PRId64
                   " gzip_us=%" PRId64 " init_us=%" PRId64 " setup_us=%" PRId64
                   " perform_us=%" PRId64 " cleanup_us=%" PRId64
                   " body_bytes=%u post_bytes=%u gzip=%d heap_before=%" PRIu32
                   " heap_after=%" PRIu32 " heap_min=%" PRIu32,
                   result,
                   err,
                   status,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   gzip_us,
                   init_us,
                   setup_us,
                   perform_us,
                   cleanup_us,
                   (unsigned)body_len,
                   (unsigned)post_body_len,
                   use_gzip ? 1 : 0,
                   heap_before,
                   HONCH_PERF_HEAP_FREE(),
                   HONCH_PERF_HEAP_MIN());
    return result;
}
