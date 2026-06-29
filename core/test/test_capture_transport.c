#include "honch/core/capture_transport.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct fake_stream {
    char writes[512];
    size_t write_count;
    size_t write_sizes[8];
    size_t write_call_count;
    const char *response;
    size_t read_offset;
    int closed;
    int close_success;
    unsigned int open_timeout_ms;
    unsigned int read_timeout_ms;
    char opened_host[96];
    uint16_t opened_port;
} fake_stream_t;

typedef struct fake_stream_env {
    fake_stream_t stream;
    honch_status_t open_status;
    honch_status_t write_status;
    honch_status_t flush_status;
    const char *connection_type;
} fake_stream_env_t;

static honch_status_t fake_open(
    void *ctx,
    const honch_capture_endpoint_t *endpoint,
    unsigned int timeout_ms,
    void **stream)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    assert(endpoint != NULL);
    assert(stream != NULL);
    if (env->open_status != HONCH_OK) {
        return env->open_status;
    }
    env->stream.open_timeout_ms = timeout_ms;
    strncpy(env->stream.opened_host, endpoint->host, sizeof(env->stream.opened_host) - 1u);
    env->stream.opened_port = endpoint->port;
    *stream = &env->stream;
    return HONCH_OK;
}

static honch_status_t fake_write(
    void *ctx,
    void *stream,
    const uint8_t *data,
    size_t data_size,
    size_t *written)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    fake_stream_t *fake = (fake_stream_t *)stream;
    assert(fake == &env->stream);
    assert(data != NULL);
    assert(written != NULL);
    if (env->write_status != HONCH_OK) {
        *written = 0u;
        return env->write_status;
    }
    assert(fake->write_call_count < sizeof(fake->write_sizes) / sizeof(fake->write_sizes[0]));
    fake->write_sizes[fake->write_call_count++] = data_size;
    if (fake->write_count + data_size < sizeof(fake->writes)) {
        memcpy(fake->writes + fake->write_count, data, data_size);
    }
    fake->write_count += data_size;
    *written = data_size;
    return HONCH_OK;
}

static int fake_read_byte(void *ctx, void *stream, unsigned int timeout_ms)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    fake_stream_t *fake = (fake_stream_t *)stream;
    assert(fake == &env->stream);
    fake->read_timeout_ms = timeout_ms;
    if (fake->response == NULL || fake->response[fake->read_offset] == '\0') {
        return -1;
    }
    return (unsigned char)fake->response[fake->read_offset++];
}

static honch_status_t fake_flush(void *ctx, void *stream)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    assert(stream == &env->stream);
    return env->flush_status;
}

static void fake_close(void *ctx, void *stream, int success)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    fake_stream_t *fake = (fake_stream_t *)stream;
    assert(fake == &env->stream);
    fake->closed++;
    fake->close_success = success;
}

static const char *fake_connection_type(void *ctx)
{
    fake_stream_env_t *env = (fake_stream_env_t *)ctx;
    return env->connection_type;
}

static honch_capture_stream_ops_t fake_stream_ops(fake_stream_env_t *env)
{
    honch_capture_stream_ops_t ops = {
        .open = fake_open,
        .write = fake_write,
        .read_byte = fake_read_byte,
        .flush = fake_flush,
        .close = fake_close,
        .connection_type = fake_connection_type,
        .ctx = env
    };
    return ops;
}

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
    assert(honch_capture_parse_endpoint("http://example.test:8001junk", &endpoint) == HONCH_ERROR_INVALID_ARGUMENT);
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

    assert(honch_capture_build_request_head(
        header,
        sizeof(header),
        &written,
        "/capture",
        "i.honch.io",
        37u,
        "project-key\r\nX-Injected: true",
        "stream-a",
        "wifi") == HONCH_ERROR_INVALID_ARGUMENT);
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

static void test_http_reason_mapping(void)
{
    assert(honch_capture_map_http_reason(401) == HONCH_REASON_AUTH_INVALID_KEY);
    assert(honch_capture_map_http_reason(503) == HONCH_REASON_HTTP_STATUS);
    assert(honch_capture_map_http_reason(400) == HONCH_REASON_HTTP_STATUS);
    assert(honch_capture_map_http_reason(429) == HONCH_REASON_HTTP_STATUS);
    assert(honch_capture_map_http_reason(204) == HONCH_REASON_NONE);
    assert(honch_capture_map_http_reason(200) == HONCH_REASON_NONE);
}

static void test_post_chunk_ex_fills_detail_on_auth_error(void)
{
    fake_stream_env_t env = {
        .stream = {.response = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"},
        .connection_type = "wifi"
    };
    honch_capture_stream_ops_t stream_ops = fake_stream_ops(&env);
    honch_capture_transport_t transport = {0};
    honch_transport_ops_t ops = {0};
    honch_capture_transport_config_t config = {.stream_ops = &stream_ops};
    assert(honch_capture_transport_init(&ops, &transport, &config) == HONCH_OK);
    /* The shared capture transport advertises the detailed variant. */
    assert(ops.post_chunk_ex != NULL);

    const uint8_t body[] = {0x01, 0x02, 0x03};
    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    honch_transport_detail_t detail = {0};
    honch_status_t status = ops.post_chunk_ex(
        ops.ctx, "http://capture.example/capture", "key", "stream",
        body, sizeof(body), &result, &detail);
    assert(status == HONCH_ERROR_REJECTED);
    assert(result == HONCH_TRANSPORT_AUTH_ERROR);
    assert(detail.http_status == 401);
    assert(detail.reason == HONCH_REASON_AUTH_INVALID_KEY);
}

static void test_capture_transport_posts_chunk_over_stream(void)
{
    fake_stream_env_t env = {
        .stream = {.response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"},
        .connection_type = "wifi"
    };
    honch_capture_stream_ops_t stream_ops = fake_stream_ops(&env);
    honch_capture_transport_t transport = {0};
    honch_transport_ops_t ops = {0};
    honch_capture_transport_config_t config = {
        .stream_ops = &stream_ops,
        .timeout_ms = 1234u,
        .max_write_bytes = 4u
    };
    assert(honch_capture_transport_init(&ops, &transport, &config) == HONCH_OK);
    assert(ops.post_chunk != NULL);
    assert(ops.ctx == &transport);

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    const uint8_t body[] = {'a', 'b', 'c', 'd', 'e'};
    assert(ops.post_chunk(ops.ctx, "https://i.honch.io/base", "project-key", "stream-a", body, sizeof(body), &result) == HONCH_OK);
    assert(result == HONCH_TRANSPORT_ACCEPTED);
    assert(env.stream.open_timeout_ms == 1234u);
    assert(env.stream.read_timeout_ms == 1234u);
    assert(strcmp(env.stream.opened_host, "i.honch.io") == 0);
    assert(env.stream.opened_port == 443u);
    assert(strstr(env.stream.writes, "POST /base/capture HTTP/1.1\r\n") == env.stream.writes);
    assert(strstr(env.stream.writes, "X-Honch-Stream-Id: stream-a\r\n") != NULL);
    assert(env.stream.write_call_count == 3u);
    assert(env.stream.write_sizes[1] == 4u);
    assert(env.stream.write_sizes[2] == 1u);
    assert(env.stream.closed == 1);
    assert(env.stream.close_success == 1);
}

static void test_capture_transport_closes_unsuccessfully_on_write_failure(void)
{
    fake_stream_env_t env = {
        .stream = {.response = "HTTP/1.1 204 No Content\r\n\r\n"},
        .write_status = HONCH_ERROR_TRANSPORT,
        .connection_type = "wifi"
    };
    honch_capture_stream_ops_t stream_ops = fake_stream_ops(&env);
    honch_capture_transport_t transport = {0};
    honch_transport_ops_t ops = {0};
    honch_capture_transport_config_t config = {.stream_ops = &stream_ops};
    assert(honch_capture_transport_init(&ops, &transport, &config) == HONCH_OK);

    honch_transport_result_t result = HONCH_TRANSPORT_ACCEPTED;
    const uint8_t body[] = {'a'};
    assert(ops.post_chunk(ops.ctx, "https://i.honch.io", "project-key", NULL, body, sizeof(body), &result) == HONCH_ERROR_TRANSPORT);
    assert(result == HONCH_TRANSPORT_RETRY);
    assert(env.stream.closed == 1);
    assert(env.stream.close_success == 0);
}

static void test_capture_transport_init_validates_required_callbacks(void)
{
    fake_stream_env_t env = {.connection_type = "wifi"};
    honch_capture_stream_ops_t stream_ops = fake_stream_ops(&env);
    honch_capture_transport_t transport = {0};
    honch_transport_ops_t ops = {0};
    honch_capture_transport_config_t config = {.stream_ops = &stream_ops};
    assert(honch_capture_transport_init(&ops, &transport, &config) == HONCH_OK);

    stream_ops.open = NULL;
    assert(honch_capture_transport_init(&ops, &transport, &config) == HONCH_ERROR_INVALID_ARGUMENT);
}

int main(void)
{
    test_endpoint_parsing_adds_capture_path();
    test_capture_request_header_formatting();
    test_http_status_mapping();
    test_http_reason_mapping();
    test_post_chunk_ex_fills_detail_on_auth_error();
    test_capture_transport_posts_chunk_over_stream();
    test_capture_transport_closes_unsuccessfully_on_write_failure();
    test_capture_transport_init_validates_required_callbacks();
    return 0;
}
