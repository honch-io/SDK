// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch_internal.h"

#include "esp_core_adapter.h"
#include "esp_http_status.h"
#include "esp_retry_after.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_tls.h"
#include "honch/core/capture_transport.h"
#include "honch_gts_roots.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS 10000u
/* Floor: a value below this cannot complete a TLS handshake + chunk POST, so a
 * misconfigured tiny timeout would fail every upload and silently age events out
 * of the bounded queue. Clamp up to a workable minimum. */
#define HONCH_ESP_MIN_TRANSPORT_TIMEOUT_MS 1000u

static const char *TAG = HONCH_LOG_SELF_TAG;

static esp_err_t honch_esp_http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL || event->event_id != HTTP_EVENT_ON_HEADER ||
        event->header_key == NULL || event->header_value == NULL ||
        strcasecmp(event->header_key, "Retry-After") != 0) {
        return ESP_OK;
    }

    honch_esp_transport_t *transport = (honch_esp_transport_t *)event->user_data;
    time_t now_seconds = time(NULL);
    uint64_t now_ms = now_seconds > 0 ? (uint64_t)now_seconds * 1000u : 0u;
    uint64_t delay_ms = 0u;
    if (honch_esp_parse_retry_after(event->header_value, now_ms, &delay_ms)) {
        transport->retry_after_ms = delay_ms;
    }
    return ESP_OK;
}

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

static void honch_esp_transport_reset_http_client(honch_esp_transport_t *transport)
{
    if (transport != NULL && transport->http_client != NULL) {
        esp_http_client_cleanup(transport->http_client);
        transport->http_client = NULL;
    }
}

static void honch_esp_transport_clear_urls(honch_esp_transport_t *transport)
{
    if (transport == NULL) {
        return;
    }
    free(transport->endpoint_url);
    free(transport->capture_url);
    transport->endpoint_url = NULL;
    transport->capture_url = NULL;
}

static honch_status_t honch_esp_transport_prepare_url(
    honch_esp_transport_t *transport,
    const char *endpoint_url,
    bool *client_reset)
{
    if (transport == NULL || endpoint_url == NULL || client_reset == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (transport->endpoint_url != NULL && strcmp(transport->endpoint_url, endpoint_url) == 0 &&
        transport->capture_url != NULL) {
        return HONCH_STATUS_OK;
    }

    honch_esp_transport_reset_http_client(transport);
    *client_reset = true;
    honch_esp_transport_clear_urls(transport);

    transport->endpoint_url = honch_strdup(endpoint_url);
    transport->capture_url = honch_esp_chunk_url(endpoint_url);
    if (transport->endpoint_url == NULL || transport->capture_url == NULL) {
        honch_esp_transport_clear_urls(transport);
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    return HONCH_STATUS_OK;
}

/* The default Honch endpoint (https://i.honch.io) is served by Google Trust
 * Services, whose chain anchors at a root that recent esp_crt_bundle builds
 * prune, so the system bundle can't verify it. We pin the GTS root set for our
 * own endpoint (honch_gts_roots.h) and keep the bundle for custom/BYO endpoints.
 * Exact-host match so look-alikes (i.honch.io.example.com) don't get pinned trust. */
static bool honch_esp_endpoint_is_default(const char *endpoint_url)
{
    static const char prefix[] = "https://i.honch.io";
    const size_t prefix_len = sizeof(prefix) - 1u;
    if (endpoint_url == NULL || strncmp(endpoint_url, prefix, prefix_len) != 0) {
        return false;
    }
    char next = endpoint_url[prefix_len];
    return next == '\0' || next == '/' || next == ':';
}

static honch_status_t honch_esp_transport_prepare_client(
    honch_esp_transport_t *transport,
    const char *endpoint_url,
    int timeout_ms,
    bool *reused_client,
    bool *client_reset)
{
    if (transport == NULL || endpoint_url == NULL || reused_client == NULL || client_reset == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = honch_esp_transport_prepare_url(transport, endpoint_url, client_reset);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    if (transport->http_client != NULL && transport->timeout_ms == timeout_ms) {
        *reused_client = true;
        return HONCH_STATUS_OK;
    }

    honch_esp_transport_reset_http_client(transport);
    *client_reset = true;

    esp_http_client_config_t config = {
        .url = transport->capture_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .keep_alive_enable = true,
        .event_handler = honch_esp_http_event_handler,
        .user_data = transport,
    };
    /* TLS trust: pin the Google Trust Services roots for the default Honch
     * endpoint (the system cert bundle can't verify its chain on recent
     * ESP-IDF); use the bundle for custom/BYO endpoints. See honch_gts_roots.h. */
    if (honch_esp_endpoint_is_default(transport->endpoint_url)) {
        config.cert_pem = HONCH_GTS_ROOTS_PEM;
    } else {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    transport->http_client = esp_http_client_init(&config);
    if (transport->http_client == NULL) {
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    transport->timeout_ms = timeout_ms;
    *reused_client = false;
    return HONCH_STATUS_OK;
}

/* Map an esp_http_client / esp-tls transport-phase failure (no usable HTTP
 * status) to a finer reason. Each esp_err_t constant is #ifdef-guarded so the
 * mapping stays correct across IDF versions where a code may be absent —
 * unknown codes fall through to the raw os_error the caller also records. */
static honch_error_reason_t honch_esp_map_err_reason(esp_err_t err)
{
    switch (err) {
#ifdef ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
        case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
            return HONCH_REASON_DNS_FAILED;
#endif
#ifdef ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST
        case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
            return HONCH_REASON_CONNECT_REFUSED;
#endif
#ifdef ESP_ERR_HTTP_CONNECT
        case ESP_ERR_HTTP_CONNECT:
            return HONCH_REASON_CONNECT_REFUSED;
#endif
#ifdef ESP_ERR_HTTP_EAGAIN
        case ESP_ERR_HTTP_EAGAIN:
            return HONCH_REASON_CONNECT_TIMEOUT;
#endif
#ifdef ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
        case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
            return HONCH_REASON_CONNECT_TIMEOUT;
#endif
#ifdef ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED
        case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
            return HONCH_REASON_TLS_HANDSHAKE;
#endif
        default:
            return HONCH_REASON_UNKNOWN;
    }
}

static honch_status_t honch_esp_post_chunk(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result,
    honch_transport_detail_t *detail)
{
    honch_esp_transport_t *transport = (honch_esp_transport_t *)ctx;
    if (transport == NULL || endpoint_url == NULL || api_key == NULL || body == NULL || body_size == 0u ||
        result == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (body_size > (size_t)INT_MAX) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *result = HONCH_TRANSPORT_RETRY;
    transport->retry_after_ms = 0u;

#ifdef HONCH_FLUSH_TIMING
    int64_t total_start_us = esp_timer_get_time();
#endif
    unsigned int configured_timeout_ms = transport->configured_timeout_ms > 0 ?
        (unsigned int)transport->configured_timeout_ms :
        HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS;
    int timeout_ms = configured_timeout_ms > (unsigned int)INT_MAX ?
        INT_MAX :
        (int)configured_timeout_ms;

#ifdef HONCH_FLUSH_TIMING
    int64_t init_start_us = esp_timer_get_time();
#endif
    bool reused_client = false;
    bool client_reset = false;
    honch_status_t prepare_status = honch_esp_transport_prepare_client(
        transport,
        endpoint_url,
        timeout_ms,
        &reused_client,
        &client_reset);
#ifdef HONCH_FLUSH_TIMING
    int64_t init_us = esp_timer_get_time() - init_start_us;
#endif
    if (prepare_status != HONCH_STATUS_OK) {
        return prepare_status;
    }

#ifdef HONCH_FLUSH_TIMING
    int64_t setup_start_us = esp_timer_get_time();
#endif
    esp_http_client_handle_t http_client = transport->http_client;
    esp_err_t err = esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(http_client, "Content-Type", "application/vnd.honch.chunk");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(http_client, "X-Honch-Project-Key", api_key);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(
            http_client,
            "X-Honch-Stream-Id",
            stream_id == NULL || stream_id[0] == '\0' ? NULL : stream_id);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(http_client, (const char *)body, (int)body_size);
    }
#ifdef HONCH_FLUSH_TIMING
    int64_t setup_us = esp_timer_get_time() - setup_start_us;
    int64_t perform_us = 0;
#endif
    if (err == ESP_OK) {
        ESP_LOGD(
            TAG,
            "HONCH_HTTP_PAYLOAD format=chunk bytes=%u url=%s stream_id=%s",
            (unsigned)body_size,
            transport->capture_url,
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
    esp_err_t clear_err = esp_http_client_set_post_field(http_client, NULL, 0);
    if (err == ESP_OK && clear_err != ESP_OK) {
        err = clear_err;
    }
#ifdef HONCH_FLUSH_TIMING
    int64_t cleanup_us = 0;
#endif

    honch_status_t return_status = honch_esp_classify_http_response(err == ESP_OK, status, result);

    if (detail != NULL) {
        if (err == ESP_OK && status > 0) {
            detail->http_status = status;
            detail->reason = honch_capture_map_http_reason(status);
        } else {
            /* esp_http_client collapses DNS / connect / TLS-handshake / cert
             * failures into the coarse ESP_ERR_HTTP_CONNECT. Recover the
             * underlying esp-tls error so the reason can distinguish them and
             * carry the finer (0x80xx) code in os_error. tls_flags != 0 means a
             * certificate verification flag was set -> a cert failure. */
            detail->os_error = (int)err;
            detail->reason = honch_esp_map_err_reason(err);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
            int tls_low = 0;
            int tls_flags = 0;
            esp_err_t tls_high =
                esp_http_client_get_and_clear_last_tls_error(http_client, &tls_low, &tls_flags);
            (void)tls_low;
            if (tls_flags != 0) {
                detail->reason = HONCH_REASON_TLS_CERT;
                detail->os_error = (int)tls_high;
            } else if (tls_high != ESP_OK && tls_high != ESP_ERR_INVALID_STATE) {
                detail->reason = honch_esp_map_err_reason(tls_high);
                detail->os_error = (int)tls_high;
            }
#endif
        }
    }

    if (err != ESP_OK || status == 0 || status == 408 || status == 409) {
#ifdef HONCH_FLUSH_TIMING
        int64_t cleanup_start_us = esp_timer_get_time();
#endif
        honch_esp_transport_reset_http_client(transport);
        client_reset = true;
#ifdef HONCH_FLUSH_TIMING
        cleanup_us = esp_timer_get_time() - cleanup_start_us;
#endif
    }

#ifdef HONCH_FLUSH_TIMING
    ESP_LOGI(
        TAG,
        "HONCH_HTTP_TIMING bytes=%u status=%d result=%d return_status=%d err=%s reused_client=%u client_reset=%u init_us=%lld setup_us=%lld perform_us=%lld cleanup_us=%lld total_us=%lld",
        (unsigned)body_size,
        status,
        (int)*result,
        (int)return_status,
        err == ESP_OK ? "ESP_OK" : esp_err_to_name(err),
        reused_client ? 1u : 0u,
        client_reset ? 1u : 0u,
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

/* Coarse variant for the required post_chunk slot: forwards without detail. */
static honch_status_t honch_esp_post_chunk_plain(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    return honch_esp_post_chunk(ctx, endpoint_url, api_key, stream_id, body, body_size, result, NULL);
}

static uint64_t honch_esp_retry_after_ms(void *ctx)
{
    honch_esp_transport_t *transport = (honch_esp_transport_t *)ctx;
    return transport == NULL ? 0u : transport->retry_after_ms;
}

honch_status_t honch_esp_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_esp_transport_t *ctx,
    unsigned int transport_timeout_ms)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_esp_transport_t) {0};
    unsigned int effective_timeout_ms = transport_timeout_ms == 0u ?
        HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS :
        transport_timeout_ms;
    if (effective_timeout_ms < HONCH_ESP_MIN_TRANSPORT_TIMEOUT_MS) {
        effective_timeout_ms = HONCH_ESP_MIN_TRANSPORT_TIMEOUT_MS;
    }
    if (effective_timeout_ms > HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS) {
        effective_timeout_ms = HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS;
    }
    ctx->configured_timeout_ms = effective_timeout_ms > (unsigned int)INT_MAX ?
        INT_MAX :
        (int)effective_timeout_ms;
    *ops = (honch_transport_ops_t) {
        .post_chunk = honch_esp_post_chunk_plain,
        .post_chunk_ex = honch_esp_post_chunk,
        .retry_after_ms = honch_esp_retry_after_ms,
        .ctx = ctx
    };
    return HONCH_STATUS_OK;
}

void honch_esp_transport_ops_deinit(honch_esp_transport_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    honch_esp_transport_reset_http_client(ctx);
    honch_esp_transport_clear_urls(ctx);
    ctx->timeout_ms = 0;
    ctx->configured_timeout_ms = 0;
    ctx->retry_after_ms = 0u;
}
