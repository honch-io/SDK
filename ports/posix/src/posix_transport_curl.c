#include "honch_internal.h"
#include "honch/honch.h"
#include "honch/posix/honch.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static honch_status_t honch_endpoint_url(const char *endpoint_url, const char *suffix, char **out)
{
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

static honch_status_t honch_chunk_url(const char *endpoint_url, char **out)
{
    return honch_endpoint_url(endpoint_url, "/capture", out);
}

static honch_status_t honch_posix_transport_post_chunk_ops(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    (void)endpoint_url;
    (void)api_key;
    honch_posix_transport_t *transport = (honch_posix_transport_t *)ctx;
    return honch_posix_transport_post_chunk(
        transport == NULL ? NULL : transport->client,
        stream_id,
        body,
        body_size,
        result);
}

honch_status_t honch_posix_transport_ops_init(honch_transport_ops_t *ops, honch_posix_transport_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_once(&honch_curl_once, honch_curl_global_init_once);

    *ctx = (honch_posix_transport_t){0};
    ctx->curl = curl_easy_init();
    if (ctx->curl == NULL) {
        return HONCH_ERROR_TRANSPORT;
    }
    *ops = (honch_transport_ops_t) {
        .post_chunk = honch_posix_transport_post_chunk_ops,
        .ctx = ctx
    };
    return HONCH_OK;
}

void honch_posix_transport_ops_deinit(honch_posix_transport_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->curl != NULL) {
        curl_easy_cleanup(ctx->curl);
        ctx->curl = NULL;
    }
    ctx->client = NULL;
}

static honch_status_t honch_transport_result_from_chunk_response(long response_code, honch_transport_result_t *result)
{
    if (response_code == 202L) {
        *result = HONCH_TRANSPORT_CHUNK_STORED;
        return HONCH_OK;
    }
    if (response_code == 204L) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_OK;
    }
    if (response_code == 0L) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }
    if (response_code == 408L) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TIMEOUT;
    }
    if (response_code == 409L) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }
    if (response_code == 429L || response_code >= 500L) {
        *result = HONCH_TRANSPORT_RETRY;
        return response_code == 429L ? HONCH_ERROR_RATE_LIMITED : HONCH_ERROR_SERVER;
    }
    if (response_code == 401L) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
        return HONCH_OK;
    }

    *result = HONCH_TRANSPORT_REJECTED;
    return HONCH_ERROR_REJECTED;
}

honch_status_t honch_posix_transport_post_chunk(
    honch_client_t *client,
    const char *stream_id,
    const unsigned char *payload,
    size_t payload_size,
    honch_transport_result_t *result)
{
    if (client == NULL || payload == NULL || payload_size == 0u || result == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *result = HONCH_TRANSPORT_RETRY;

    char *url = NULL;
    honch_status_t status = honch_chunk_url(client->endpoint_url, &url);
    if (status != HONCH_OK) {
        return status;
    }

#ifdef HONCH_TESTING
    if (honch_test_transport != NULL) {
        long response_code = 0L;
        status = honch_test_transport(
            url,
            client->api_key,
            stream_id,
            payload,
            payload_size,
            "identity",
            honch_test_transport_userdata,
            &response_code);
        free(url);
        if (status != HONCH_OK) {
            *result = HONCH_TRANSPORT_RETRY;
            return status;
        }

        return honch_transport_result_from_chunk_response(response_code, result);
    }
#endif

    honch_posix_transport_t *transport =
        client->transport == NULL ? NULL : (honch_posix_transport_t *)client->transport->ctx;
    CURL *curl = transport == NULL ? NULL : (CURL *)transport->curl;
    if (curl == NULL) {
        free(url);
        return HONCH_ERROR_TRANSPORT;
    }
    curl_easy_reset(curl);

    struct curl_slist *headers = NULL;
    struct curl_slist *next_header = curl_slist_append(headers, "Content-Type: application/vnd.honch.chunk");
    if (next_header == NULL) {
        free(url);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    size_t key_header_size = 0u;
    status = honch_size_add3(strlen("X-Honch-Project-Key: "), strlen(client->api_key), 1u, &key_header_size);
    if (status != HONCH_OK) {
        curl_slist_free_all(headers);
        free(url);
        return status;
    }
    char *key_header = (char *)malloc(key_header_size);
    if (key_header == NULL) {
        curl_slist_free_all(headers);
        free(url);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    snprintf(key_header, key_header_size, "X-Honch-Project-Key: %s", client->api_key);
    next_header = curl_slist_append(headers, key_header);
    free(key_header);
    if (next_header == NULL) {
        curl_slist_free_all(headers);
        free(url);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    if (stream_id != NULL && stream_id[0] != '\0') {
        size_t stream_header_size = 0u;
        status = honch_size_add3(strlen("X-Honch-Stream-Id: "), strlen(stream_id), 1u, &stream_header_size);
        if (status != HONCH_OK) {
            curl_slist_free_all(headers);
            free(url);
            return status;
        }
        char *stream_header = (char *)malloc(stream_header_size);
        if (stream_header == NULL) {
            curl_slist_free_all(headers);
            free(url);
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        snprintf(stream_header, stream_header_size, "X-Honch-Stream-Id: %s", stream_id);
        next_header = curl_slist_append(headers, stream_header);
        free(stream_header);
        if (next_header == NULL) {
            curl_slist_free_all(headers);
            free(url);
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        headers = next_header;
    }

    if (payload_size > (size_t)LONG_MAX) {
        curl_slist_free_all(headers);
        free(url);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload_size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, honch_discard_response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)client->transport_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)client->transport_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /* Pin TLS verification explicitly so it can never be silently weakened by a
     * build/libcurl default change: verify the peer certificate chain and that
     * the certificate matches the host. The endpoint defaults to https; an
     * integrator may still configure a plaintext (e.g. loopback) endpoint, in
     * which case these options are inert because no TLS handshake occurs. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Only ever speak HTTP(S): a malformed or hostile endpoint string must not
     * be able to make libcurl use file://, scp://, gopher://, etc. Redirects
     * are not followed (CURLOPT_FOLLOWLOCATION is left off), so the project-key
     * header cannot be redirected to another host or scheme. */
    /* CURLOPT_PROTOCOLS_STR is an enum constant, not a macro, so it cannot be
     * probed with defined(): gate on the libcurl version. The string form was
     * added in 7.85.0, which is also where the legacy CURLOPT_PROTOCOLS became
     * deprecated (a -Werror=deprecated-declarations failure on newer curl). */
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    CURLcode code = curl_easy_perform(curl);
    long response_code = 0L;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    curl_slist_free_all(headers);
    free(url);

    if (code != CURLE_OK) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    return honch_transport_result_from_chunk_response(response_code, result);
}

#ifdef HONCH_TESTING
void honch_test_set_transport(honch_test_transport_fn transport, void *userdata)
{
    honch_test_transport = transport;
    honch_test_transport_userdata = userdata;
}
#endif
