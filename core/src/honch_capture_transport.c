#include "honch/core/capture_transport.h"

#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_CAPTURE_DEFAULT_MAX_WRITE_BYTES 1024u
#define HONCH_CAPTURE_REQUEST_HEAD_BYTES 384u
#define HONCH_CAPTURE_RESPONSE_LINE_BYTES 512u

static int honch_present(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static int honch_capture_safe_field(const char *value, int allow_empty)
{
    if (value == NULL) {
        return allow_empty;
    }
    if (!allow_empty && value[0] == '\0') {
        return 0;
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor <= 0x20u || *cursor == 0x7fu) {
            return 0;
        }
    }
    return 1;
}

static honch_status_t honch_copy_segment(char *out, size_t out_size, const char *begin, size_t length)
{
    if (out == NULL || out_size == 0u || begin == NULL || length >= out_size) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out, begin, length);
    out[length] = '\0';
    return HONCH_OK;
}

honch_status_t honch_capture_parse_endpoint(const char *endpoint_url, honch_capture_endpoint_t *out)
{
    if (!honch_present(endpoint_url) || out == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    const char *cursor = NULL;
    if (strncmp(endpoint_url, "https://", 8u) == 0) {
        out->tls = 1;
        out->port = 443u;
        cursor = endpoint_url + 8u;
    } else if (strncmp(endpoint_url, "http://", 7u) == 0) {
        out->tls = 0;
        out->port = 80u;
        cursor = endpoint_url + 7u;
    } else {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    const char *host_begin = cursor;
    while (*cursor != '\0' && *cursor != ':' && *cursor != '/') {
        cursor++;
    }
    if (cursor == host_begin) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_copy_segment(out->host, sizeof(out->host), host_begin, (size_t)(cursor - host_begin));
    if (status != HONCH_OK) {
        return status;
    }

    if (*cursor == ':') {
        cursor++;
        char *end = NULL;
        unsigned long parsed_port = strtoul(cursor, &end, 10);
        if (end == cursor || parsed_port == 0u || parsed_port > 65535u ||
            (*end != '\0' && *end != '/')) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        out->port = (uint16_t)parsed_port;
        cursor = end;
    }

    const char *path_begin = (*cursor == '/') ? cursor : "";
    size_t path_len = strlen(path_begin);
    while (path_len > 0u && path_begin[path_len - 1u] == '/') {
        path_len--;
    }

    if (path_len == 0u) {
        return honch_copy_segment(out->path, sizeof(out->path), "/capture", sizeof("/capture") - 1u);
    }

    /* path_len + strlen("/capture") must leave room for the trailing NUL. */
    if (path_len + (sizeof("/capture") - 1u) >= sizeof(out->path)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out->path, path_begin, path_len);
    memcpy(out->path + path_len, "/capture", sizeof("/capture"));
    return HONCH_OK;
}

honch_status_t honch_capture_build_request_head(
    char *buffer,
    size_t buffer_size,
    size_t *written,
    const char *path,
    const char *host,
    size_t body_size,
    const char *api_key,
    const char *stream_id,
    const char *connection_type)
{
    if (buffer == NULL || buffer_size == 0u || written == NULL ||
        !honch_present(path) || !honch_present(host) ||
        !honch_present(api_key) || !honch_present(connection_type) ||
        !honch_capture_safe_field(path, 0) ||
        !honch_capture_safe_field(host, 0) ||
        !honch_capture_safe_field(api_key, 0) ||
        !honch_capture_safe_field(stream_id, 1) ||
        !honch_capture_safe_field(connection_type, 0)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    int has_stream_id = honch_present(stream_id);
    int n = snprintf(buffer, buffer_size,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/vnd.honch.chunk\r\n"
        "Content-Length: %u\r\n"
        "X-Honch-Project-Key: %s\r\n"
        "%s%s%s"
        "X-Connection-Type: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        host,
        (unsigned)body_size,
        api_key,
        has_stream_id ? "X-Honch-Stream-Id: " : "",
        has_stream_id ? stream_id : "",
        has_stream_id ? "\r\n" : "",
        connection_type);
    if (n < 0 || (size_t)n >= buffer_size) {
        *written = 0u;
        buffer[0] = '\0';
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    *written = (size_t)n;
    return HONCH_OK;
}

honch_status_t honch_capture_map_http_status(int status_code, honch_transport_result_t *result)
{
    if (result == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (status_code == 204) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_OK;
    }
    if (status_code == 202) {
        *result = HONCH_TRANSPORT_CHUNK_STORED;
        return HONCH_OK;
    }
    if (status_code >= 200 && status_code <= 299) {
        *result = HONCH_TRANSPORT_ACCEPTED;
        return HONCH_OK;
    }
    if (status_code == 401) {
        *result = HONCH_TRANSPORT_AUTH_ERROR;
        return HONCH_ERROR_REJECTED;
    }
    if (status_code == 408 || status_code == 409 || status_code == 429 ||
        (status_code >= 500 && status_code <= 599)) {
        *result = HONCH_TRANSPORT_RETRY;
        return HONCH_ERROR_TRANSPORT;
    }
    if (status_code >= 400 && status_code <= 499) {
        *result = HONCH_TRANSPORT_REJECTED;
        return HONCH_ERROR_REJECTED;
    }

    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_ERROR_TRANSPORT;
}

honch_error_reason_t honch_capture_map_http_reason(int status_code)
{
    if (status_code == 401) {
        return HONCH_REASON_AUTH_INVALID_KEY;
    }
    if (status_code >= 200 && status_code <= 299) {
        return HONCH_REASON_NONE;
    }
    return HONCH_REASON_HTTP_STATUS;
}

static honch_status_t honch_capture_write_all(
    honch_capture_transport_t *transport,
    void *stream,
    const uint8_t *data,
    size_t data_size,
    int limit_chunk_size,
    honch_transport_result_t *result)
{
    if (transport == NULL || stream == NULL || data == NULL || result == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t offset = 0u;
    while (offset < data_size) {
        size_t remaining = data_size - offset;
        size_t chunk = remaining;
        if (limit_chunk_size && transport->max_write_bytes > 0u && chunk > transport->max_write_bytes) {
            chunk = transport->max_write_bytes;
        }

        size_t written = 0u;
        honch_status_t status = transport->stream_ops.write(
            transport->stream_ops.ctx,
            stream,
            data + offset,
            chunk,
            &written);
        if (status != HONCH_OK || written == 0u || written > chunk) {
            *result = HONCH_TRANSPORT_RETRY;
            return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
        }

        status = transport->stream_ops.flush(transport->stream_ops.ctx, stream);
        if (status != HONCH_OK) {
            *result = HONCH_TRANSPORT_RETRY;
            return status;
        }
        offset += written;
    }

    return HONCH_OK;
}

static honch_status_t honch_capture_read_line(
    honch_capture_transport_t *transport,
    void *stream,
    char *line,
    size_t line_size)
{
    if (transport == NULL || stream == NULL || line == NULL || line_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t used = 0u;
    while (used + 1u < line_size) {
        int byte = transport->stream_ops.read_byte(
            transport->stream_ops.ctx,
            stream,
            transport->timeout_ms);
        if (byte < 0) {
            line[0] = '\0';
            return HONCH_ERROR_TRANSPORT;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            line[used] = '\0';
            return HONCH_OK;
        }
        line[used++] = (char)byte;
    }

    line[0] = '\0';
    return HONCH_ERROR_OUT_OF_MEMORY;
}

static honch_status_t honch_capture_read_status(
    honch_capture_transport_t *transport,
    void *stream,
    int *status_code)
{
    if (status_code == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *status_code = 0;

    char line[HONCH_CAPTURE_RESPONSE_LINE_BYTES];
    honch_status_t status = honch_capture_read_line(transport, stream, line, sizeof(line));
    if (status != HONCH_OK) {
        return status;
    }
    if (strncmp(line, "HTTP/", 5u) != 0) {
        return HONCH_ERROR_TRANSPORT;
    }

    char *space = strchr(line, ' ');
    if (space == NULL) {
        return HONCH_ERROR_TRANSPORT;
    }
    char *end = NULL;
    long parsed = strtol(space + 1, &end, 10);
    if (end == space + 1 || parsed < 100 || parsed > 999) {
        return HONCH_ERROR_TRANSPORT;
    }
    *status_code = (int)parsed;

    do {
        status = honch_capture_read_line(transport, stream, line, sizeof(line));
        if (status != HONCH_OK) {
            return status;
        }
    } while (line[0] != '\0');

    return HONCH_OK;
}

static honch_status_t honch_capture_transport_post_chunk_ex(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result,
    honch_transport_detail_t *detail)
{
    honch_capture_transport_t *transport = (honch_capture_transport_t *)ctx;
    if (transport == NULL || endpoint_url == NULL || api_key == NULL ||
        body == NULL || body_size == 0u || result == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *result = HONCH_TRANSPORT_RETRY;

    honch_capture_endpoint_t endpoint;
    honch_status_t status = honch_capture_parse_endpoint(endpoint_url, &endpoint);
    if (status != HONCH_OK) {
        *result = HONCH_TRANSPORT_REJECTED;
        if (detail != NULL) {
            detail->reason = HONCH_REASON_INVALID_CONFIG;
        }
        return status;
    }

    const char *connection_type = transport->stream_ops.connection_type(transport->stream_ops.ctx);
    char header[HONCH_CAPTURE_REQUEST_HEAD_BYTES];
    size_t header_size = 0u;
    status = honch_capture_build_request_head(
        header,
        sizeof(header),
        &header_size,
        endpoint.path,
        endpoint.host,
        body_size,
        api_key,
        stream_id,
        connection_type);
    if (status != HONCH_OK) {
        return status;
    }

    void *stream = NULL;
    status = transport->stream_ops.open(
        transport->stream_ops.ctx,
        &endpoint,
        transport->timeout_ms,
        &stream);
    if (status != HONCH_OK || stream == NULL) {
        return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
    }

    status = honch_capture_write_all(transport, stream, (const uint8_t *)header, header_size, 0, result);
    if (status == HONCH_OK) {
        status = honch_capture_write_all(transport, stream, body, body_size, 1, result);
    }

    int http_status = 0;
    if (status == HONCH_OK) {
        status = honch_capture_read_status(transport, stream, &http_status);
    }
    if (status == HONCH_OK) {
        status = honch_capture_map_http_status(http_status, result);
    }

    /* Fill the optional detail with whatever the stream layer surfaced. The
     * shared capture transport works over an abstract stream, so it can only
     * classify the HTTP outcome precisely; ports with a native transport
     * (curl CURLcode, esp_err_t) refine connect/TLS/DNS reasons themselves. */
    if (detail != NULL) {
        detail->http_status = http_status;
        if (http_status > 0) {
            detail->reason = honch_capture_map_http_reason(http_status);
        } else {
            /* No HTTP response: a transport-phase failure (open/write/read). */
            detail->reason = HONCH_REASON_WRITE_FAILED;
        }
    }

    transport->stream_ops.close(
        transport->stream_ops.ctx,
        stream,
        status == HONCH_OK ? 1 : 0);
    return status;
}

static honch_status_t honch_capture_transport_post_chunk(
    void *ctx,
    const char *endpoint_url,
    const char *api_key,
    const char *stream_id,
    const uint8_t *body,
    size_t body_size,
    honch_transport_result_t *result)
{
    return honch_capture_transport_post_chunk_ex(
        ctx, endpoint_url, api_key, stream_id, body, body_size, result, NULL);
}

honch_status_t honch_capture_transport_init(
    honch_transport_ops_t *transport_ops,
    honch_capture_transport_t *transport,
    const honch_capture_transport_config_t *config)
{
    if (transport_ops == NULL || transport == NULL || config == NULL ||
        config->stream_ops == NULL ||
        config->stream_ops->open == NULL ||
        config->stream_ops->write == NULL ||
        config->stream_ops->read_byte == NULL ||
        config->stream_ops->flush == NULL ||
        config->stream_ops->close == NULL ||
        config->stream_ops->connection_type == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    memset(transport, 0, sizeof(*transport));
    transport->stream_ops = *config->stream_ops;
    transport->timeout_ms = config->timeout_ms == 0u ?
        HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS :
        config->timeout_ms;
    transport->max_write_bytes = config->max_write_bytes == 0u ?
        HONCH_CAPTURE_DEFAULT_MAX_WRITE_BYTES :
        config->max_write_bytes;

    memset(transport_ops, 0, sizeof(*transport_ops));
    transport_ops->post_chunk = honch_capture_transport_post_chunk;
    transport_ops->post_chunk_ex = honch_capture_transport_post_chunk_ex;
    transport_ops->ctx = transport;
    return HONCH_OK;
}
