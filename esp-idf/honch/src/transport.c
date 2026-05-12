// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "transport.h"

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
    mz_ulong bound = mz_compressBound(body_len);
    if (bound > SIZE_MAX - 18u) {
        return false;
    }

    uint8_t *compressed = malloc((size_t)bound + 18u);
    if (!compressed) {
        ESP_LOGW(TAG, "Gzip allocation failed; sending raw CBOR");
        return false;
    }

    static const uint8_t header[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
    };
    memcpy(compressed, header, sizeof(header));

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (const unsigned char *)body;
    stream.avail_in = body_len;
    stream.next_out = compressed + sizeof(header);
    stream.avail_out = bound;

    int ret = mz_deflateInit2(
        &stream,
        MZ_DEFAULT_COMPRESSION,
        MZ_DEFLATED,
        -MZ_DEFAULT_WINDOW_BITS,
        9,
        MZ_DEFAULT_STRATEGY);
    if (ret != MZ_OK) {
        ESP_LOGW(TAG, "Gzip init failed (%d); sending raw CBOR", ret);
        free(compressed);
        return false;
    }

    ret = mz_deflate(&stream, MZ_FINISH);
    if (ret != MZ_STREAM_END) {
        ESP_LOGW(TAG, "Gzip deflate failed (%d); sending raw CBOR", ret);
        mz_deflateEnd(&stream);
        free(compressed);
        return false;
    }

    size_t deflated_size = stream.total_out;
    mz_deflateEnd(&stream);

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

    size_t compressed_size = sizeof(header) + deflated_size + 8u;
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

#ifdef CONFIG_HONCH_ENABLE_GZIP
    if (body_len >= CONFIG_HONCH_GZIP_MIN_BYTES &&
        honch_gzip_payload(body, body_len, &compressed, &post_body_len)) {
        post_body = compressed;
        use_gzip = true;
#ifdef CONFIG_HONCH_LOG_VERBOSE
        ESP_LOGI(TAG, "Gzipped CBOR batch: %u -> %u bytes",
                 (unsigned)body_len, (unsigned)post_body_len);
#endif
    }
#endif

    esp_http_client_config_t http_config = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(compressed);
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    esp_http_client_set_header(client, "Content-Type", "application/cbor");
    if (use_gzip) {
        esp_http_client_set_header(client, "Content-Encoding", "gzip");
    }
    esp_http_client_set_post_field(client, (const char *)post_body, post_body_len);

    esp_err_t err = esp_http_client_perform(client);
    honch_transport_result_t result;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        result = HONCH_TRANSPORT_NETWORK_ERROR;
    } else {
        int status = esp_http_client_get_status_code(client);
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

    esp_http_client_cleanup(client);
    free(compressed);
    return result;
}
