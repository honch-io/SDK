#include "honch_internal.h"

#include <curl/curl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HONCH_TESTING
static honch_test_transport_fn honch_test_transport = NULL;
static void *honch_test_transport_userdata = NULL;
#endif

static pthread_once_t honch_curl_once = PTHREAD_ONCE_INIT;

static void honch_curl_global_init_once(void)
{
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t honch_discard_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    if (size != 0u && nmemb > SIZE_MAX / size) {
        return 0u;
    }
    return size * nmemb;
}

static honch_status_t honch_batch_url(const char *endpoint_url, char **out)
{
    const char *suffix = "/batch";
    size_t endpoint_length = strlen(endpoint_url);
    while (endpoint_length > 0u && endpoint_url[endpoint_length - 1u] == '/') {
        endpoint_length--;
    }

    size_t total = 0u;
    honch_status_t status = honch_size_add3(endpoint_length, strlen(suffix), 1u, &total);
    if (status != HONCH_OK) {
        return status;
    }

    char *url = (char *)malloc(total);
    if (url == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    snprintf(url, total, "%.*s%s", (int)endpoint_length, endpoint_url, suffix);
    *out = url;
    return HONCH_OK;
}

static honch_status_t honch_map_response(long response_code, honch_http_result_t *result)
{
    if (response_code >= 200L && response_code < 300L) {
        *result = HONCH_HTTP_OK;
        return HONCH_OK;
    }

    if (response_code == 429L || response_code >= 500L) {
        *result = HONCH_HTTP_RETRY;
        return response_code == 429L ? HONCH_ERROR_RATE_LIMITED : HONCH_ERROR_SERVER;
    }

    *result = HONCH_HTTP_REJECTED;
    return HONCH_ERROR_REJECTED;
}

static honch_status_t honch_gzip_payload(const char *payload, unsigned char **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0u;

    size_t payload_size = strlen(payload);
    if (payload_size > UINT_MAX) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    int zstatus = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        MAX_WBITS + 16,
        8,
        Z_DEFAULT_STRATEGY);
    if (zstatus != Z_OK) {
        return HONCH_ERROR_IO;
    }

    uLong bound = deflateBound(&stream, (uLong)payload_size);
    if (bound > UINT_MAX) {
        deflateEnd(&stream);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    unsigned char *compressed = (unsigned char *)malloc((size_t)bound);
    if (compressed == NULL) {
        deflateEnd(&stream);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    stream.next_in = (Bytef *)payload;
    stream.avail_in = (uInt)payload_size;
    stream.next_out = compressed;
    stream.avail_out = (uInt)bound;

    zstatus = deflate(&stream, Z_FINISH);
    if (zstatus != Z_STREAM_END) {
        free(compressed);
        deflateEnd(&stream);
        return HONCH_ERROR_IO;
    }

    *out_size = (size_t)stream.total_out;
    *out = compressed;
    deflateEnd(&stream);
    return HONCH_OK;
}

#ifdef HONCH_TESTING
honch_status_t honch_test_gzip_payload(const char *payload, unsigned char **out, size_t *out_size)
{
    if (payload == NULL || out == NULL || out_size == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return honch_gzip_payload(payload, out, out_size);
}
#endif

honch_status_t honch_transport_post_batch(
    honch_client_t *client,
    const char *payload,
    honch_http_result_t *result)
{
    char *url = NULL;
    honch_status_t status = honch_batch_url(client->endpoint_url, &url);
    if (status != HONCH_OK) {
        return status;
    }

#ifdef HONCH_TESTING
    if (honch_test_transport != NULL) {
        long response_code = 0L;
        status = honch_test_transport(url, client->api_key, payload, honch_test_transport_userdata, &response_code);
        free(url);
        if (status != HONCH_OK) {
            *result = HONCH_HTTP_RETRY;
            return status;
        }
        return honch_map_response(response_code, result);
    }
#endif

    unsigned char *compressed_payload = NULL;
    size_t compressed_payload_size = 0u;
    status = honch_gzip_payload(payload, &compressed_payload, &compressed_payload_size);
    if (status != HONCH_OK) {
        free(url);
        return status;
    }

    pthread_once(&honch_curl_once, honch_curl_global_init_once);

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(compressed_payload);
        free(url);
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    honch_buffer_t auth;
    size_t auth_capacity = 0u;
    status = honch_size_add(strlen(client->api_key), 32u, &auth_capacity);
    if (status == HONCH_OK) {
        status = honch_buffer_init(&auth, auth_capacity);
    }
    if (status != HONCH_OK) {
        free(compressed_payload);
        free(url);
        curl_easy_cleanup(curl);
        return status;
    }

    status = honch_buffer_append(&auth, "Authorization: Bearer ");
    if (status == HONCH_OK) {
        status = honch_buffer_append(&auth, client->api_key);
    }
    if (status != HONCH_OK) {
        free(compressed_payload);
        honch_buffer_free(&auth);
        free(url);
        curl_easy_cleanup(curl);
        return status;
    }

    struct curl_slist *headers = NULL;
    struct curl_slist *next_header = curl_slist_append(headers, "Content-Type: application/json");
    if (next_header == NULL) {
        free(compressed_payload);
        honch_buffer_free(&auth);
        free(url);
        curl_easy_cleanup(curl);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    next_header = curl_slist_append(headers, "Content-Encoding: gzip");
    if (next_header == NULL) {
        curl_slist_free_all(headers);
        free(compressed_payload);
        honch_buffer_free(&auth);
        free(url);
        curl_easy_cleanup(curl);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    next_header = curl_slist_append(headers, auth.data);
    if (next_header == NULL) {
        curl_slist_free_all(headers);
        free(compressed_payload);
        honch_buffer_free(&auth);
        free(url);
        curl_easy_cleanup(curl);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)compressed_payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)compressed_payload_size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, honch_discard_response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)client->transport_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode code = curl_easy_perform(curl);
    long response_code = 0L;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    curl_slist_free_all(headers);
    free(compressed_payload);
    honch_buffer_free(&auth);
    free(url);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    return honch_map_response(response_code, result);
}

#ifdef HONCH_TESTING
void honch_test_set_transport(honch_test_transport_fn transport, void *userdata)
{
    honch_test_transport = transport;
    honch_test_transport_userdata = userdata;
}
#endif
