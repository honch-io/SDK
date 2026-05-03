// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "transport.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "miniz.h"

static const char *TAG = "honch";

static char s_url[256];

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

honch_transport_result_t honch_transport_send(const char *body, size_t body_len)
{
    if (!body || body_len == 0) {
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    // Gzip compress the body
    mz_ulong compressed_size = mz_compressBound(body_len);
    // Use a larger buffer for gzip header/trailer
    size_t gzip_buf_size = compressed_size + 18; // gzip header (10) + trailer (8)
    uint8_t *compressed = malloc(gzip_buf_size);
    if (!compressed) {
        ESP_LOGE(TAG, "Failed to allocate compression buffer");
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    // Write gzip header
    uint8_t gzip_header[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    memcpy(compressed, gzip_header, 10);

    // Deflate (raw, no zlib wrapper)
    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (const unsigned char *)body;
    stream.avail_in = body_len;
    stream.next_out = compressed + 10;
    stream.avail_out = gzip_buf_size - 18;

    int ret = mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                               -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    if (ret != MZ_OK) {
        ESP_LOGE(TAG, "deflateInit2 failed: %d", ret);
        free(compressed);
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    ret = mz_deflate(&stream, MZ_FINISH);
    if (ret != MZ_STREAM_END) {
        ESP_LOGE(TAG, "deflate failed: %d", ret);
        mz_deflateEnd(&stream);
        free(compressed);
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    size_t deflated_size = stream.total_out;
    mz_deflateEnd(&stream);

    // Write gzip trailer (CRC32 + original size)
    uint32_t crc = mz_crc32(MZ_CRC32_INIT, (const unsigned char *)body, body_len);
    uint8_t *trailer = compressed + 10 + deflated_size;
    trailer[0] = crc & 0xFF;
    trailer[1] = (crc >> 8) & 0xFF;
    trailer[2] = (crc >> 16) & 0xFF;
    trailer[3] = (crc >> 24) & 0xFF;
    trailer[4] = body_len & 0xFF;
    trailer[5] = (body_len >> 8) & 0xFF;
    trailer[6] = (body_len >> 16) & 0xFF;
    trailer[7] = (body_len >> 24) & 0xFF;

    size_t total_size = 10 + deflated_size + 8;

#ifdef CONFIG_HONCH_LOG_VERBOSE
    ESP_LOGI(TAG, "Compressed %u -> %u bytes (%.0f%%)",
             (unsigned)body_len, (unsigned)total_size,
             100.0 * (1.0 - (double)total_size / body_len));
#endif

    // HTTP POST
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

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Content-Encoding", "gzip");
    esp_http_client_set_post_field(client, (const char *)compressed, total_size);

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
        } else {
            ESP_LOGE(TAG, "Server error (HTTP %d)", status);
            result = HONCH_TRANSPORT_SERVER_ERROR;
        }
    }

    esp_http_client_cleanup(client);
    free(compressed);
    return result;
}
