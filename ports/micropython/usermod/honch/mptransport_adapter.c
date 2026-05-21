#include "honch_micropython.h"

#include <stdlib.h>
#include <string.h>

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

    mp_obj_t requests = mp_import_name(MP_QSTR_urequests, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t post = mp_load_attr(requests, MP_QSTR_post);
    mp_obj_t headers = mp_obj_new_dict(3);
    mp_obj_dict_store(headers, mp_obj_new_str("Content-Type", 12), mp_obj_new_str("application/vnd.honch.chunk", 27));
    mp_obj_dict_store(headers, mp_obj_new_str("X-Honch-Project-Key", 19), mp_obj_new_str(api_key, strlen(api_key)));
    if (stream_id != NULL && stream_id[0] != '\0') {
        mp_obj_dict_store(headers, mp_obj_new_str("X-Honch-Stream-Id", 17), mp_obj_new_str(stream_id, strlen(stream_id)));
    }

    mp_obj_t args[5] = {
        mp_obj_new_str(url, strlen(url)),
        MP_OBJ_NEW_QSTR(MP_QSTR_data),
        mp_obj_new_bytes(body, body_size),
        MP_OBJ_NEW_QSTR(MP_QSTR_headers),
        headers,
    };
    mp_obj_t response = mp_call_function_n_kw(post, 1, 2, args);
    mp_obj_t status_obj = mp_load_attr(response, MP_QSTR_status_code);
    int http_status = mp_obj_get_int(status_obj);
    (void)honch_micropython_call_noarg_attr(response, MP_QSTR_close);

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
