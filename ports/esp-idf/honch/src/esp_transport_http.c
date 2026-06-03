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
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS 30000u

static const char *TAG = "honch";

static int honch_esp_month_index(const char *month)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month == NULL) {
        return -1;
    }
    for (int i = 0; i < 12; i++) {
        if (strcasecmp(month, months[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool honch_esp_parse_retry_after_seconds(const char *value, uint64_t *delay_ms)
{
    if (value == NULL || delay_ms == NULL) {
        return false;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value < '0' || *value > '9') {
        return false;
    }

    uint64_t seconds = 0u;
    while (*value >= '0' && *value <= '9') {
        uint64_t digit = (uint64_t)(*value - '0');
        if (seconds > (UINT64_MAX - digit) / 10u) {
            seconds = UINT64_MAX / 1000u;
            break;
        }
        seconds = (seconds * 10u) + digit;
        value++;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value != '\0') {
        return false;
    }

    *delay_ms = seconds > UINT64_MAX / 1000u ? UINT64_MAX : seconds * 1000u;
    return true;
}

static int64_t honch_esp_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2u;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    int adjusted_month = (int)month + ((int)month > 2 ? -3 : 9);
    unsigned day_of_year = (unsigned)((153 * adjusted_month + 2) / 5) + day - 1u;
    unsigned day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool honch_esp_parse_retry_after_http_date(const char *value, uint64_t now_ms, uint64_t *delay_ms)
{
    if (value == NULL || delay_ms == NULL || now_ms == 0u) {
        return false;
    }

    char weekday[4] = {0};
    char month_name[4] = {0};
    char gmt[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    int matched = sscanf(
        value,
        "%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d %3[A-Za-z]%n",
        weekday,
        &day,
        month_name,
        &year,
        &hour,
        &minute,
        &second,
        gmt,
        &consumed);
    if (matched != 8 || value[consumed] != '\0' || strcasecmp(gmt, "GMT") != 0) {
        return false;
    }

    int month = honch_esp_month_index(month_name);
    if (year < 1970 || month < 0 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }

    int64_t days = honch_esp_days_from_civil(year, (unsigned)month + 1u, (unsigned)day);
    if (days < 0) {
        return false;
    }
    uint64_t target_ms = ((uint64_t)days * 86400u + (uint64_t)hour * 3600u +
        (uint64_t)minute * 60u + (uint64_t)second) * 1000u;
    if (target_ms <= now_ms) {
        *delay_ms = 0u;
        return true;
    }
    *delay_ms = target_ms - now_ms;
    return true;
}

static bool honch_esp_parse_retry_after(const char *value, uint64_t now_ms, uint64_t *delay_ms)
{
    return honch_esp_parse_retry_after_seconds(value, delay_ms) ||
        honch_esp_parse_retry_after_http_date(value, now_ms, delay_ms);
}

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
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .event_handler = honch_esp_http_event_handler,
        .user_data = transport,
    };
    transport->http_client = esp_http_client_init(&config);
    if (transport->http_client == NULL) {
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    transport->timeout_ms = timeout_ms;
    *reused_client = false;
    return HONCH_STATUS_OK;
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
        ESP_LOGI(
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
    if (effective_timeout_ms > HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS) {
        effective_timeout_ms = HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS;
    }
    ctx->configured_timeout_ms = effective_timeout_ms > (unsigned int)INT_MAX ?
        INT_MAX :
        (int)effective_timeout_ms;
    *ops = (honch_transport_ops_t) {
        .post_chunk = honch_esp_post_chunk,
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
