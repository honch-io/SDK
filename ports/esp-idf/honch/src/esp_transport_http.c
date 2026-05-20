// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "honch";

#ifdef CONFIG_HONCH_ENABLE_GZIP
static bool honch_esp_gzip_payload(
    const uint8_t *body,
    size_t body_size,
    uint8_t **out,
    size_t *out_size)
{
    static const uint8_t header[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
    };

    size_t deflated_size = 0u;
    uint8_t *deflated = (uint8_t *)tdefl_compress_mem_to_heap(
        body,
        body_size,
        &deflated_size,
        TDEFL_DEFAULT_MAX_PROBES);
    if (deflated == NULL) {
        return false;
    }

    if (deflated_size > SIZE_MAX - sizeof(header) - 8u) {
        free(deflated);
        return false;
    }

    size_t compressed_size = sizeof(header) + deflated_size + 8u;
    uint8_t *compressed = (uint8_t *)malloc(compressed_size);
    if (compressed == NULL) {
        free(deflated);
        return false;
    }

    memcpy(compressed, header, sizeof(header));
    memcpy(compressed + sizeof(header), deflated, deflated_size);
    free(deflated);

    uint32_t crc = mz_crc32(MZ_CRC32_INIT, body, body_size);
    uint8_t *trailer = compressed + sizeof(header) + deflated_size;
    trailer[0] = (uint8_t)(crc & 0xffu);
    trailer[1] = (uint8_t)((crc >> 8u) & 0xffu);
    trailer[2] = (uint8_t)((crc >> 16u) & 0xffu);
    trailer[3] = (uint8_t)((crc >> 24u) & 0xffu);
    trailer[4] = (uint8_t)(body_size & 0xffu);
    trailer[5] = (uint8_t)((body_size >> 8u) & 0xffu);
    trailer[6] = (uint8_t)((body_size >> 16u) & 0xffu);
    trailer[7] = (uint8_t)((body_size >> 24u) & 0xffu);

    if (compressed_size >= body_size) {
        free(compressed);
        return false;
    }

    *out = compressed;
    *out_size = compressed_size;
    return true;
}
#endif

static honch_status_t honch_esp_post_batch(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const uint8_t *body,
    size_t body_size,
    const char *content_encoding,
    honch_transport_result_t *result)
{
    (void)ctx;
    (void)api_key;
    if (endpoint_url == NULL || body == NULL || body_size == 0u || result == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *result = HONCH_TRANSPORT_RETRY;
    const uint8_t *post_body = body;
    size_t post_body_size = body_size;
    const char *post_encoding = content_encoding;
    uint8_t *compressed = NULL;

#ifdef CONFIG_HONCH_ENABLE_GZIP
    if (post_encoding == NULL && body_size >= CONFIG_HONCH_GZIP_MIN_BYTES &&
        honch_esp_gzip_payload(body, body_size, &compressed, &post_body_size)) {
        post_body = compressed;
        post_encoding = "gzip";
    }
#endif

    esp_http_client_config_t config = {
        .url = endpoint_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(compressed);
        return HONCH_STATUS_ERROR_TRANSPORT;
    }

    esp_http_client_set_header(client, "Content-Type", "application/cbor");
    if (post_encoding != NULL && strcmp(post_encoding, "gzip") == 0) {
        esp_http_client_set_header(client, "Content-Encoding", "gzip");
    }
    esp_http_client_set_post_field(client, (const char *)post_body, post_body_size);

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    free(compressed);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }

    if (status >= 200 && status < 300) {
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
        .post_batch = honch_esp_post_batch,
        .ctx = ctx
    };
    return HONCH_STATUS_OK;
}
