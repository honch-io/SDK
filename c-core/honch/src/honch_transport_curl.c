#include "honch_internal.h"

#include <curl/curl.h>
#include <limits.h>
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

#ifdef HONCH_TESTING
    if (honch_test_transport != NULL) {
        long response_code = 0L;
        status = honch_test_transport(
            url,
            client->api_key,
            payload,
            payload_size,
            "identity",
            honch_test_transport_userdata,
            &response_code);
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
        free(url);
        *result = HONCH_HTTP_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }

    struct curl_slist *headers = NULL;
    struct curl_slist *next_header = curl_slist_append(headers, "Content-Type: application/cbor");
    if (next_header == NULL) {
        free(url);
        curl_easy_cleanup(curl);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    headers = next_header;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload_size);
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
