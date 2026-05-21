#include "honch_internal.h"
#include "honch/honch.h"
#include "honch/posix/honch.h"

#include <curl/curl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HONCH_HAVE_ZLIB
#include <zlib.h>
#endif

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
    size_t suffix_length = strlen(suffix);
    size_t endpoint_length = strlen(endpoint_url);
    while (endpoint_length > 0u && endpoint_url[endpoint_length - 1u] == '/') {
        endpoint_length--;
    }

    size_t total = 0u;
    honch_status_t status = honch_size_add3(endpoint_length, suffix_length, 1u, &total);
    if (status != HONCH_OK) {
        return status;
    }

    char *url = (char *)malloc(total);
    if (url == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    memcpy(url, endpoint_url, endpoint_length);
    memcpy(url + endpoint_length, suffix, suffix_length);
    url[endpoint_length + suffix_length] = '\0';
    *out = url;
    return HONCH_OK;
}

static honch_status_t honch_map_response(long response_code, honch_http_result_t *result)
{
    if (response_code >= 200L && response_code < 300L) {
        *result = HONCH_HTTP_OK;
        return HONCH_OK;
    }

    if (response_code == 408L) {
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_TIMEOUT;
    }

    if (response_code == 429L || response_code >= 500L) {
        *result = HONCH_HTTP_RETRY;
        return response_code == 429L ? HONCH_ERROR_RATE_LIMITED : HONCH_ERROR_SERVER;
    }

    *result = HONCH_HTTP_REJECTED;
    return HONCH_ERROR_REJECTED;
}

static honch_status_t honch_posix_transport_post_batch(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const uint8_t *body,
    size_t body_size,
    const char *content_encoding,
    honch_transport_result_t *result)
{
    (void)endpoint_url;
    (void)api_key;
    (void)content_encoding;
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || result == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_http_result_t http_result = HONCH_HTTP_RETRY;
    honch_status_t status = honch_transport_post_batch(client, body, body_size, &http_result);
    switch (http_result) {
        case HONCH_HTTP_OK:
            *result = HONCH_TRANSPORT_ACCEPTED;
            break;
        case HONCH_HTTP_REJECTED:
            *result = HONCH_TRANSPORT_REJECTED;
            break;
        case HONCH_HTTP_RETRY:
        default:
            *result = HONCH_TRANSPORT_RETRY;
            break;
    }
    return status;
}

honch_status_t honch_posix_transport_ops_init(honch_transport_ops_t *ops, honch_posix_transport_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_posix_transport_t) {
        .client = NULL
    };
    *ops = (honch_transport_ops_t) {
        .post_batch = honch_posix_transport_post_batch,
        .ctx = NULL
    };
    return HONCH_OK;
}

#ifdef HONCH_HAVE_ZLIB
static honch_status_t honch_gzip_payload(
    const unsigned char *payload,
    size_t payload_size,
    unsigned char **out,
    size_t *out_size)
{
    if (payload_size > UINT_MAX) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uLong bound = compressBound((uLong)payload_size);
    if (bound > SIZE_MAX - 18u) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    unsigned char *compressed = (unsigned char *)malloc((size_t)bound + 18u);
    if (compressed == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    static const unsigned char header[10] = {
        0x1fu, 0x8bu, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u
    };
    memcpy(compressed, header, sizeof(header));

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)payload;
    stream.avail_in = (uInt)payload_size;
    stream.next_out = compressed + sizeof(header);
    stream.avail_out = (uInt)bound;

    int ret = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        -MAX_WBITS,
        9,
        Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        free(compressed);
        return HONCH_ERROR_TRANSPORT;
    }

    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        free(compressed);
        return HONCH_ERROR_TRANSPORT;
    }

    size_t deflated_size = stream.total_out;
    deflateEnd(&stream);

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, payload, (uInt)payload_size);
    unsigned char *trailer = compressed + sizeof(header) + deflated_size;
    trailer[0] = (unsigned char)(crc & 0xffu);
    trailer[1] = (unsigned char)((crc >> 8u) & 0xffu);
    trailer[2] = (unsigned char)((crc >> 16u) & 0xffu);
    trailer[3] = (unsigned char)((crc >> 24u) & 0xffu);
    trailer[4] = (unsigned char)(payload_size & 0xffu);
    trailer[5] = (unsigned char)((payload_size >> 8u) & 0xffu);
    trailer[6] = (unsigned char)((payload_size >> 16u) & 0xffu);
    trailer[7] = (unsigned char)((payload_size >> 24u) & 0xffu);

    *out = compressed;
    *out_size = sizeof(header) + deflated_size + 8u;
    return HONCH_OK;
}
#endif

honch_status_t honch_transport_post_batch(
    honch_client_t *client,
    const unsigned char *payload,
    size_t payload_size,
    honch_http_result_t *result)
{
    char *url = NULL;
    honch_status_t status = honch_batch_url(client->endpoint_url, &url);
    if (status != HONCH_OK) {
        return status;
    }

    const unsigned char *body = payload;
    size_t body_size = payload_size;
    const char *content_encoding = "identity";
    unsigned char *compressed = NULL;

#ifdef HONCH_HAVE_ZLIB
    if (client->gzip_enabled && payload_size >= client->gzip_min_bytes) {
        size_t compressed_size = 0u;
        if (honch_gzip_payload(payload, payload_size, &compressed, &compressed_size) == HONCH_OK &&
            compressed_size < payload_size) {
            body = compressed;
            body_size = compressed_size;
            content_encoding = "gzip";
        } else {
            free(compressed);
            compressed = NULL;
        }
    }
#endif

#ifdef HONCH_TESTING
    if (honch_test_transport != NULL) {
        long response_code = 0L;
        status = honch_test_transport(
            url,
            client->api_key,
            body,
            body_size,
            content_encoding,
            honch_test_transport_userdata,
            &response_code);
        free(compressed);
        free(url);
        if (status != HONCH_OK) {
            *result = HONCH_HTTP_RETRY;
            return status;
        }
        return honch_map_response(response_code, result);
    }
#endif

    pthread_once(&honch_curl_once, honch_curl_global_init_once);

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(compressed);
        free(url);
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    struct curl_slist *headers = NULL;
    struct curl_slist *next_header = curl_slist_append(headers, "Content-Type: application/cbor");
    if (next_header == NULL) {
        free(compressed);
        free(url);
        curl_easy_cleanup(curl);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;
    if (strcmp(content_encoding, "gzip") == 0) {
        next_header = curl_slist_append(headers, "Content-Encoding: gzip");
        if (next_header == NULL) {
            curl_slist_free_all(headers);
            free(compressed);
            free(url);
            curl_easy_cleanup(curl);
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        headers = next_header;
    }

    if (body_size > (size_t)LONG_MAX) {
        curl_slist_free_all(headers);
        free(compressed);
        free(url);
        curl_easy_cleanup(curl);
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_size);
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
    free(compressed);
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
