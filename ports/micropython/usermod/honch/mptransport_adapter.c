#include "honch_micropython.h"

#include <stdlib.h>
#include <string.h>

static honch_status_t honch_mp_batch_url(const char *endpoint_url, char **out)
{
    if (endpoint_url == NULL || out == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    const char *suffix = "/batch";
    size_t endpoint_length = strlen(endpoint_url);
    while (endpoint_length > 0u && endpoint_url[endpoint_length - 1u] == '/') {
        endpoint_length--;
    }
    size_t suffix_length = strlen(suffix);
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

static honch_status_t honch_mp_post_batch(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const uint8_t *body,
    size_t body_size,
    const char *content_encoding,
    honch_transport_result_t *result)
{
    (void)api_key;
    honch_micropython_transport_t *transport = (honch_micropython_transport_t *)ctx;
    if (transport == NULL || endpoint_url == NULL || body == NULL || body_size == 0u || result == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *result = HONCH_TRANSPORT_RETRY;

    char *url = NULL;
    honch_status_t status = honch_mp_batch_url(endpoint_url, &url);
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
    mp_obj_t headers = mp_obj_new_dict(2);
    mp_obj_dict_store(headers, mp_obj_new_str("Content-Type", 12), mp_obj_new_str("application/cbor", 16));
    if (content_encoding != NULL && strcmp(content_encoding, "gzip") == 0) {
        mp_obj_dict_store(headers, mp_obj_new_str("Content-Encoding", 16), mp_obj_new_str("gzip", 4));
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

    if (http_status >= 200 && http_status < 300) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_STATUS_OK;
    }
    if (http_status == 401) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
        return HONCH_STATUS_OK;
    }
    if (http_status == 408) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_STATUS_ERROR_TIMEOUT;
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
    unsigned int timeout_ms,
    int disable_gzip,
    size_t gzip_min_bytes)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *ctx = (honch_micropython_transport_t) {
        .requests_module = mp_const_none,
        .timeout_ms = timeout_ms,
        .disable_gzip = disable_gzip,
        .gzip_min_bytes = gzip_min_bytes,
    };
    *ops = (honch_transport_ops_t) {
        .post_batch = honch_mp_post_batch,
        .ctx = ctx,
    };
    return HONCH_STATUS_OK;
}
