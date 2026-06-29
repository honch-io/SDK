#include "honch_micropython.h"
#include "honch_internal.h"  /* route this file's malloc/free to the GC heap (HONCH_USE_MP_ALLOC) */

#include <stdlib.h>
#include <string.h>

#define HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS 10000u

static honch_status_t honch_mp_endpoint_url(const char *endpoint_url, const char *suffix, char **out)
{
    if (endpoint_url == NULL || suffix == NULL || out == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    size_t endpoint_length = strlen(endpoint_url);
    while (endpoint_length > 0u && endpoint_url[endpoint_length - 1u] == '/') {
        endpoint_length--;
    }
    size_t suffix_length = strlen(suffix);
    if (endpoint_length > SIZE_MAX - suffix_length - 1u) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    char *url = (char *)malloc(endpoint_length + suffix_length + 1u);
    if (url == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    memcpy(url, endpoint_url, endpoint_length);
    memcpy(url + endpoint_length, suffix, suffix_length);
    url[endpoint_length + suffix_length] = '\0';
    *out = url;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_chunk_url(const char *endpoint_url, char **out)
{
    return honch_mp_endpoint_url(endpoint_url, "/capture", out);
}

static honch_status_t honch_mp_post_chunk(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    honch_micropython_transport_t *transport = (honch_micropython_transport_t *)ctx;
    if (transport == NULL || endpoint_url == NULL || api_key == NULL ||
        body == NULL || body_size == 0u || result == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *result = HONCH_TRANSPORT_RETRY;

    char *url = NULL;
    honch_status_t status = honch_mp_chunk_url(endpoint_url, &url);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        free(url);
        return HONCH_STATUS_ERROR_TRANSPORT;
    }

    /* Use the frozen honch_transport helper instead of urequests.post: it does a
     * non-blocking connect bounded by a hard deadline so a flaky / transitional
     * Wi-Fi link raises promptly (mapped to TRANSPORT/TIMEOUT -> core retry/backoff)
     * rather than blocking connect() forever and wedging this single-threaded VM.
     * urequests/requests' settimeout does NOT bound connect()/handshake on rp2. */
    mp_obj_t transport_mod = mp_import_name(qstr_from_str("honch_transport"), mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t post = mp_load_attr(transport_mod, qstr_from_str("post_chunk"));
    mp_obj_t headers = mp_obj_new_dict(3);
    mp_obj_dict_store(headers, mp_obj_new_str("Content-Type", 12), mp_obj_new_str("application/vnd.honch.chunk", 27));
    mp_obj_dict_store(headers, mp_obj_new_str("X-Honch-Project-Key", 19), mp_obj_new_str(api_key, strlen(api_key)));
    if (stream_id != NULL && stream_id[0] != '\0') {
        mp_obj_dict_store(headers, mp_obj_new_str("X-Honch-Stream-Id", 17), mp_obj_new_str(stream_id, strlen(stream_id)));
    }

    /* honch_transport.post_chunk(url, body, headers, timeout_ms) -> int HTTP status */
    mp_obj_t args[4] = {
        mp_obj_new_str(url, strlen(url)),
        mp_obj_new_bytes(body, body_size),
        headers,
        mp_obj_new_int_from_uint(transport->timeout_ms),
    };
    mp_obj_t status_obj = mp_call_function_n_kw(post, 4, 0, args);
    int http_status = mp_obj_get_int(status_obj);

    nlr_pop();
    free(url);

    if (http_status == 202) {
        *result = HONCH_TRANSPORT_CHUNK_STORED;
        return HONCH_STATUS_OK;
    }
    if (http_status == 204) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_STATUS_OK;
    }
    if (http_status == 0) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    if (http_status == 401) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
        return HONCH_STATUS_OK;
    }
    if (http_status == 408) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TIMEOUT;
    }
    if (http_status == 409) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TRANSPORT;
    }
    if (http_status == 429) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_RATE_LIMITED;
    }
    if (http_status >= 500) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_SERVER;
    }
    *result = HONCH_TRANSPORT_REJECTED;
    return HONCH_STATUS_OK;
}

honch_status_t honch_micropython_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_micropython_transport_t *ctx,
    unsigned int timeout_ms)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (timeout_ms == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (timeout_ms > HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS) {
        timeout_ms = HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS;
    }
    *ctx = (honch_micropython_transport_t) {
        .requests_module = mp_const_none,
        .timeout_ms = timeout_ms,
    };
    *ops = (honch_transport_ops_t) {
        .post_chunk = honch_mp_post_chunk,
        .ctx = ctx,
    };
    return HONCH_STATUS_OK;
}
