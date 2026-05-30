#include "honch_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

honch_status_t honch_size_add(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    *out = left + right;
    return HONCH_OK;
}

honch_status_t honch_size_add3(size_t first, size_t second, size_t third, size_t *out)
{
    size_t partial = 0u;
    honch_status_t status = honch_size_add(first, second, &partial);
    if (status != HONCH_OK) {
        return status;
    }
    return honch_size_add(partial, third, out);
}

honch_status_t honch_size_mul(size_t left, size_t right, size_t *out)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    *out = left * right;
    return HONCH_OK;
}

bool honch_is_blank(const char *value)
{
    if (value == NULL) {
        return true;
    }

    while (*value != '\0') {
        if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r') {
            return false;
        }
        value++;
    }
    return true;
}

bool honch_utf8_is_valid(const char *value, size_t length)
{
    if (value == NULL && length > 0u) {
        return false;
    }

    const unsigned char *bytes = (const unsigned char *)value;
    size_t i = 0u;
    while (i < length) {
        unsigned char byte = bytes[i];
        if (byte <= 0x7fu) {
            i++;
            continue;
        }

        uint32_t codepoint = 0u;
        size_t continuation_count = 0u;
        if (byte >= 0xc2u && byte <= 0xdfu) {
            codepoint = byte & 0x1fu;
            continuation_count = 1u;
        } else if (byte >= 0xe0u && byte <= 0xefu) {
            codepoint = byte & 0x0fu;
            continuation_count = 2u;
        } else if (byte >= 0xf0u && byte <= 0xf4u) {
            codepoint = byte & 0x07u;
            continuation_count = 3u;
        } else {
            return false;
        }

        if (continuation_count > length - i - 1u) {
            return false;
        }
        for (size_t j = 0u; j < continuation_count; j++) {
            unsigned char continuation = bytes[i + 1u + j];
            if ((continuation & 0xc0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6u) | (uint32_t)(continuation & 0x3fu);
        }

        if ((continuation_count == 1u && codepoint < 0x80u) ||
            (continuation_count == 2u && codepoint < 0x800u) ||
            (continuation_count == 3u && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu) {
            return false;
        }

        i += continuation_count + 1u;
    }
    return true;
}

char *honch_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }

    size_t length = strlen(value);
    size_t total = 0u;
    if (honch_size_add(length, 1u, &total) != HONCH_OK) {
        return NULL;
    }

    char *copy = (char *)malloc(total);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length + 1u);
    return copy;
}

void honch_free_client_fields(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

    free(client->api_key);
    free(client->endpoint_url);
    free(client->device_id);
    free(client->device_model);
    free(client->firmware_version);
    free(client->environment);
    free(client->sdk_platform);
    free(client->queue_directory);
    free(client->pending_directory);
    free(client->dead_directory);
    free(client->state_directory);
    free(client->distinct_id);
    free(client->session_id);
}

bool honch_property_key_is_reserved(const char *key)
{
    static const char *reserved[] = {
        "$battery_level",
        "distinct_id",
        "$device_id",
        "$device_model",
        "$environment",
        "$firmware_version",
        "$sdk_platform",
        "$sdk_version",
        "$session_id",
        "$wifi_rssi"
    };

    if (key == NULL) {
        return false;
    }

    for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(key, reserved[i]) == 0) {
            return true;
        }
    }
    return false;
}

honch_status_t honch_buffer_init(honch_buffer_t *buffer, size_t initial_capacity)
{
    if (initial_capacity == 0u) {
        initial_capacity = 1u;
    }

    buffer->data = (char *)malloc(initial_capacity);
    if (buffer->data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    buffer->data[0] = '\0';
    buffer->length = 0u;
    buffer->capacity = initial_capacity;
    return HONCH_OK;
}

void honch_buffer_free(honch_buffer_t *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0u;
    buffer->capacity = 0u;
}

honch_status_t honch_buffer_reserve(honch_buffer_t *buffer, size_t needed)
{
    if (needed <= buffer->capacity) {
        return HONCH_OK;
    }

    size_t next = buffer->capacity == 0u ? 64u : buffer->capacity;
    while (next < needed) {
        if (next > ((size_t)-1) / 2u) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        next *= 2u;
    }

    char *data = (char *)realloc(buffer->data, next);
    if (data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    buffer->data = data;
    buffer->capacity = next;
    return HONCH_OK;
}

honch_status_t honch_buffer_append_n(honch_buffer_t *buffer, const char *value, size_t length)
{
    size_t needed = 0u;
    honch_status_t status = honch_size_add3(buffer->length, length, 1u, &needed);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_reserve(buffer, needed);
    if (status != HONCH_OK) {
        return status;
    }

    memcpy(buffer->data + buffer->length, value, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return HONCH_OK;
}

honch_status_t honch_buffer_append(honch_buffer_t *buffer, const char *value)
{
    return honch_buffer_append_n(buffer, value, strlen(value));
}

honch_status_t honch_buffer_appendf(honch_buffer_t *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (required < 0) {
        va_end(args);
        return HONCH_ERROR_IO;
    }

    size_t needed = 0u;
    honch_status_t status = honch_size_add3(buffer->length, (size_t)required, 1u, &needed);
    if (status != HONCH_OK) {
        va_end(args);
        return status;
    }

    status = honch_buffer_reserve(buffer, needed);
    if (status != HONCH_OK) {
        va_end(args);
        return status;
    }

    int written = vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
    va_end(args);
    if (written < 0) {
        return HONCH_ERROR_IO;
    }

    buffer->length += (size_t)written;
    return HONCH_OK;
}
