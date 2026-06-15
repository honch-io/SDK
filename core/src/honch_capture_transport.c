#include "honch/core/capture_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int honch_present(const char *value)
{
    return value != NULL && value[0] != '\0';
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
        if (end == cursor || parsed_port == 0u || parsed_port > 65535u) {
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
        return honch_copy_segment(out->path, sizeof(out->path), "/capture", 8u);
    }

    if (path_len + 8u >= sizeof(out->path)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out->path, path_begin, path_len);
    memcpy(out->path + path_len, "/capture", 9u);
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
        !honch_present(api_key) || !honch_present(connection_type)) {
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
