// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "transport.h"

#include <stdio.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

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

honch_transport_result_t honch_transport_send(const uint8_t *body, size_t body_len)
{
    if (!body || body_len == 0) {
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

#ifdef CONFIG_HONCH_LOG_VERBOSE
    ESP_LOGI(TAG, "Sending CBOR batch: %u bytes", (unsigned)body_len);
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
        return HONCH_TRANSPORT_NETWORK_ERROR;
    }

    esp_http_client_set_header(client, "Content-Type", "application/cbor");
    esp_http_client_set_post_field(client, (const char *)body, body_len);

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
    return result;
}
