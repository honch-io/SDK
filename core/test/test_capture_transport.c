#include "honch/core/capture_transport.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_endpoint_parsing_adds_capture_path(void)
{
    honch_capture_endpoint_t endpoint = {0};
    assert(honch_capture_parse_endpoint("https://i.honch.io", &endpoint) == HONCH_OK);
    assert(endpoint.tls);
    assert(strcmp(endpoint.host, "i.honch.io") == 0);
    assert(endpoint.port == 443u);
    assert(strcmp(endpoint.path, "/capture") == 0);

    assert(honch_capture_parse_endpoint("http://127.0.0.1:8001/base/", &endpoint) == HONCH_OK);
    assert(!endpoint.tls);
    assert(strcmp(endpoint.host, "127.0.0.1") == 0);
    assert(endpoint.port == 8001u);
    assert(strcmp(endpoint.path, "/base/capture") == 0);

    assert(honch_capture_parse_endpoint("ftp://example.test", &endpoint) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_capture_parse_endpoint("https://", &endpoint) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_capture_request_header_formatting(void)
{
    char header[384] = {0};
    size_t written = 0u;
    assert(honch_capture_build_request_head(
        header,
        sizeof(header),
        &written,
        "/capture",
        "i.honch.io",
        37u,
        "project-key",
        "stream-a",
        "wifi") == HONCH_OK);
    assert(written == strlen(header));
    assert(strstr(header, "POST /capture HTTP/1.1\r\n") == header);
    assert(strstr(header, "Host: i.honch.io\r\n") != NULL);
    assert(strstr(header, "Content-Type: application/vnd.honch.chunk\r\n") != NULL);
    assert(strstr(header, "Content-Length: 37\r\n") != NULL);
    assert(strstr(header, "X-Honch-Project-Key: project-key\r\n") != NULL);
    assert(strstr(header, "X-Honch-Stream-Id: stream-a\r\n") != NULL);
    assert(strstr(header, "X-Connection-Type: wifi\r\n") != NULL);
    assert(strstr(header, "Connection: close\r\n\r\n") != NULL);

    assert(honch_capture_build_request_head(
        header,
        sizeof(header),
        &written,
        "/base/capture",
        "relay.local",
        1025u,
        "project-key",
        "",
        "ble") == HONCH_OK);
    assert(strstr(header, "X-Honch-Stream-Id") == NULL);
    assert(strstr(header, "X-Connection-Type: ble\r\n") != NULL);

    char tiny[16] = {0};
    assert(honch_capture_build_request_head(
        tiny,
        sizeof(tiny),
        &written,
        "/capture",
        "i.honch.io",
        37u,
        "project-key",
        "stream-a",
        "wifi") == HONCH_ERROR_OUT_OF_MEMORY);
}

static void assert_status_maps(
    int status_code,
    honch_status_t expected_status,
    honch_transport_result_t expected_result)
{
    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    assert(honch_capture_map_http_status(status_code, &result) == expected_status);
    assert(result == expected_result);
}

static void test_http_status_mapping(void)
{
    assert_status_maps(202, HONCH_OK, HONCH_TRANSPORT_CHUNK_STORED);
    assert_status_maps(204, HONCH_OK, HONCH_TRANSPORT_ACCEPTED);
    assert_status_maps(200, HONCH_OK, HONCH_TRANSPORT_ACCEPTED);
    assert_status_maps(299, HONCH_OK, HONCH_TRANSPORT_ACCEPTED);
    assert_status_maps(401, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_AUTH_ERROR);
    assert_status_maps(408, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
    assert_status_maps(409, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
    assert_status_maps(429, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
    assert_status_maps(500, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
    assert_status_maps(503, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
    assert_status_maps(400, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
    assert_status_maps(404, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
    assert_status_maps(0, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
}

int main(void)
{
    test_endpoint_parsing_adds_capture_path();
    test_capture_request_header_formatting();
    test_http_status_mapping();
    return 0;
}
