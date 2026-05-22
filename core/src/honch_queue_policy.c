#include "honch_internal.h"
#include "honch/core/wire_v2.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_WIRE_V2_MAX_FRAME_BYTES 4096u
#define HONCH_WIRE_V2_MAX_PARSED_PROPERTIES 64u
#define HONCH_WIRE_V2_MAX_PARSED_DEPTH 8u

typedef struct honch_cbor_event_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
} honch_cbor_event_reader_t;

typedef struct honch_wire_v2_parsed_event {
    char *event_name;
    char *distinct_id;
    char *session_id;
    uint64_t timestamp_ms;
    honch_wire_v2_property_t properties[HONCH_WIRE_V2_MAX_PARSED_PROPERTIES];
    size_t property_count;
} honch_wire_v2_parsed_event_t;

static honch_status_t honch_core_queue_depth(honch_client_t *client, size_t *depth)
{
    if (client == NULL || depth == NULL || client->storage == NULL || client->storage->queue_depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_depth(client->storage->ctx, depth);
}

static honch_status_t honch_core_queue_peek(honch_client_t *client, honch_storage_reader_t *reader)
{
    if (client == NULL || reader == NULL || client->storage == NULL || client->storage->queue_peek == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_peek(client->storage->ctx, reader);
}

static honch_status_t honch_core_queue_consume(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_consume == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_consume(client->storage->ctx, sequence);
}

static honch_status_t honch_core_queue_dead_letter(honch_client_t *client, uint64_t sequence)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_dead_letter == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_dead_letter(client->storage->ctx, sequence);
}

static honch_status_t honch_core_read_queue_event(
    honch_client_t *client,
    const honch_storage_reader_t *reader,
    honch_payload_t *event)
{
    event->data = NULL;
    event->length = 0u;

    if (client == NULL || reader == NULL || reader->read == NULL ||
        reader->total_size == 0u || reader->total_size > client->max_event_bytes) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *data = (uint8_t *)malloc(reader->total_size);
    if (data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_status_t status = reader->read(reader->ctx, 0u, data, reader->total_size);
    if (status != HONCH_OK) {
        free(data);
        return status;
    }

    if (!honch_cbor_validate_event(data, reader->total_size)) {
        free(data);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    event->data = data;
    event->length = reader->total_size;
    return HONCH_OK;
}

static honch_status_t honch_cbor_event_read_u64(honch_cbor_event_reader_t *reader, uint8_t additional, uint64_t *out)
{
    if (additional < 24u) {
        *out = additional;
        return HONCH_OK;
    }

    size_t bytes = 0u;
    if (additional == 24u) {
        bytes = 1u;
    } else if (additional == 25u) {
        bytes = 2u;
    } else if (additional == 26u) {
        bytes = 4u;
    } else if (additional == 27u) {
        bytes = 8u;
    } else {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (bytes > reader->length - reader->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t value = 0u;
    for (size_t i = 0u; i < bytes; i++) {
        value = (value << 8u) | reader->data[reader->offset++];
    }
    *out = value;
    return HONCH_OK;
}

static honch_status_t honch_cbor_event_read_type(
    honch_cbor_event_reader_t *reader,
    uint8_t expected_major,
    uint64_t *value)
{
    if (reader->offset >= reader->length) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t initial = reader->data[reader->offset++];
    uint8_t major = initial & 0xe0u;
    uint8_t additional = initial & 0x1fu;
    if (major != expected_major) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return honch_cbor_event_read_u64(reader, additional, value);
}

static honch_status_t honch_cbor_event_read_text(
    honch_cbor_event_reader_t *reader,
    const char **out,
    size_t *out_length)
{
    uint64_t length = 0u;
    honch_status_t status = honch_cbor_event_read_type(reader, 0x60u, &length);
    if (status != HONCH_OK || length > SIZE_MAX || (size_t)length > reader->length - reader->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *out = (const char *)reader->data + reader->offset;
    *out_length = (size_t)length;
    reader->offset += (size_t)length;
    return HONCH_OK;
}

static honch_status_t honch_cbor_event_read_text_copy(
    honch_cbor_event_reader_t *reader,
    char **out,
    size_t max_length,
    bool allow_empty)
{
    const char *text = NULL;
    size_t text_length = 0u;
    honch_status_t status = honch_cbor_event_read_text(reader, &text, &text_length);
    if (status != HONCH_OK || (!allow_empty && text_length == 0u) || text_length > max_length) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    char *copy = (char *)malloc(text_length + 1u);
    if (copy == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, text_length);
    copy[text_length] = '\0';
    *out = copy;
    return HONCH_OK;
}

static honch_status_t honch_cbor_event_read_int(honch_cbor_event_reader_t *reader, uint64_t *out)
{
    if (reader->offset >= reader->length) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t initial = reader->data[reader->offset++];
    if ((initial & 0xe0u) != 0x00u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return honch_cbor_event_read_u64(reader, initial & 0x1fu, out);
}

static bool honch_text_equals(const char *text, size_t text_length, const char *expected)
{
    size_t expected_length = strlen(expected);
    return text_length == expected_length && memcmp(text, expected, expected_length) == 0;
}

static bool honch_wire_property_is_promoted_context(const char *key)
{
    static const char *promoted[] = {
        "$device_id",
        "$device_model",
        "$firmware_version",
        "$sdk_platform",
        "$sdk_version",
        "$environment",
        "$session_id"
    };

    for (size_t i = 0u; i < sizeof(promoted) / sizeof(promoted[0]); i++) {
        if (strcmp(key, promoted[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void honch_wire_v2_value_free(honch_wire_v2_value_t *value)
{
    if (value == NULL) {
        return;
    }

    if (value->type == HONCH_WIRE_V2_VALUE_TYPE_STRING) {
        free((void *)value->string_value);
    } else if (value->type == HONCH_WIRE_V2_VALUE_TYPE_BYTES) {
        free((void *)value->bytes.data);
    } else if (value->type == HONCH_WIRE_V2_VALUE_TYPE_ARRAY) {
        honch_wire_v2_value_t *items = (honch_wire_v2_value_t *)value->array.items;
        for (size_t i = 0u; i < value->array.count; i++) {
            honch_wire_v2_value_free(&items[i]);
        }
        free(items);
    } else if (value->type == HONCH_WIRE_V2_VALUE_TYPE_MAP) {
        honch_wire_v2_map_pair_t *entries = (honch_wire_v2_map_pair_t *)value->map.entries;
        for (size_t i = 0u; i < value->map.count; i++) {
            free((void *)entries[i].key);
            honch_wire_v2_value_free(&entries[i].value);
        }
        free(entries);
    }

    memset(value, 0, sizeof(*value));
}

static honch_status_t honch_parse_queued_cbor_property_value(
    honch_cbor_event_reader_t *reader,
    honch_wire_v2_value_t *value,
    size_t depth)
{
    if (reader->offset >= reader->length || value == NULL || depth > HONCH_WIRE_V2_MAX_PARSED_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t initial = reader->data[reader->offset];
    uint8_t major = initial & 0xe0u;
    uint8_t additional = initial & 0x1fu;

    if (major == 0x00u) {
        reader->offset++;
        uint64_t uint_value = 0u;
        honch_status_t status = honch_cbor_event_read_u64(reader, additional, &uint_value);
        if (status != HONCH_OK) {
            return status;
        }
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
            .uint_value = uint_value
        };
        return HONCH_OK;
    }

    if (major == 0x20u) {
        reader->offset++;
        uint64_t magnitude = 0u;
        honch_status_t status = honch_cbor_event_read_u64(reader, additional, &magnitude);
        if (status != HONCH_OK || magnitude > (uint64_t)INT64_MAX) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_INT,
            .int_value = -1 - (int64_t)magnitude
        };
        return HONCH_OK;
    }

    if (major == 0x40u) {
        uint64_t length = 0u;
        honch_status_t status = honch_cbor_event_read_type(reader, 0x40u, &length);
        if (status != HONCH_OK || length > SIZE_MAX || (size_t)length > reader->length - reader->offset) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }

        uint8_t *copy = NULL;
        if (length > 0u) {
            copy = (uint8_t *)malloc((size_t)length);
            if (copy == NULL) {
                return HONCH_ERROR_OUT_OF_MEMORY;
            }
            memcpy(copy, reader->data + reader->offset, (size_t)length);
        }
        reader->offset += (size_t)length;
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_BYTES,
            .bytes = {
                .data = copy,
                .size = (size_t)length
            }
        };
        return HONCH_OK;
    }

    if (major == 0x60u) {
        const char *text = NULL;
        size_t text_length = 0u;
        honch_status_t status = honch_cbor_event_read_text(reader, &text, &text_length);
        if (status != HONCH_OK) {
            return status;
        }
        char *copy = (char *)malloc(text_length + 1u);
        if (copy == NULL) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        memcpy(copy, text, text_length);
        copy[text_length] = '\0';
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
            .string_value = copy,
            .string_size = text_length
        };
        return HONCH_OK;
    }

    if (major == 0x80u) {
        uint64_t count = 0u;
        honch_status_t status = honch_cbor_event_read_type(reader, 0x80u, &count);
        if (status != HONCH_OK || count > SIZE_MAX / sizeof(honch_wire_v2_value_t)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }

        honch_wire_v2_value_t *items = NULL;
        if (count > 0u) {
            items = (honch_wire_v2_value_t *)calloc((size_t)count, sizeof(*items));
            if (items == NULL) {
                return HONCH_ERROR_OUT_OF_MEMORY;
            }
        }

        for (uint64_t i = 0u; i < count; i++) {
            status = honch_parse_queued_cbor_property_value(reader, &items[i], depth + 1u);
            if (status != HONCH_OK) {
                for (uint64_t j = 0u; j < i; j++) {
                    honch_wire_v2_value_free(&items[j]);
                }
                free(items);
                return status;
            }
        }

        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
            .array = {
                .items = items,
                .count = (size_t)count
            }
        };
        return HONCH_OK;
    }

    if (major == 0xa0u) {
        uint64_t count = 0u;
        honch_status_t status = honch_cbor_event_read_type(reader, 0xa0u, &count);
        if (status != HONCH_OK || count > SIZE_MAX / sizeof(honch_wire_v2_map_pair_t)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }

        honch_wire_v2_map_pair_t *entries = NULL;
        if (count > 0u) {
            entries = (honch_wire_v2_map_pair_t *)calloc((size_t)count, sizeof(*entries));
            if (entries == NULL) {
                return HONCH_ERROR_OUT_OF_MEMORY;
            }
        }

        for (uint64_t i = 0u; i < count; i++) {
            char *key = NULL;
            status = honch_cbor_event_read_text_copy(reader, &key, HONCH_MAX_EVENT_NAME, false);
            if (status == HONCH_OK) {
                entries[i].key = key;
                status = honch_parse_queued_cbor_property_value(reader, &entries[i].value, depth + 1u);
            }
            if (status != HONCH_OK) {
                free(key);
                for (uint64_t j = 0u; j < i; j++) {
                    free((void *)entries[j].key);
                    honch_wire_v2_value_free(&entries[j].value);
                }
                free(entries);
                return status;
            }
        }

        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
            .map = {
                .entries = entries,
                .count = (size_t)count
            }
        };
        return HONCH_OK;
    }

    if (initial == 0xf9u) {
        if (reader->length - reader->offset < 3u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        reader->offset++;
        uint16_t half = ((uint16_t)reader->data[reader->offset] << 8u) |
            (uint16_t)reader->data[reader->offset + 1u];
        reader->offset += 2u;

        uint32_t sign = (uint32_t)(half & 0x8000u) << 16u;
        uint32_t exponent = (half >> 10u) & 0x1fu;
        uint32_t fraction = half & 0x03ffu;
        uint32_t bits = 0u;
        if (exponent == 0u) {
            if (fraction == 0u) {
                bits = sign;
            } else {
                int32_t normalized_exponent = -14;
                while ((fraction & 0x0400u) == 0u) {
                    fraction <<= 1u;
                    normalized_exponent--;
                }
                fraction &= 0x03ffu;
                bits = sign |
                    ((uint32_t)(normalized_exponent + 127) << 23u) |
                    (fraction << 13u);
            }
        } else if (exponent == 0x1fu) {
            bits = sign | 0x7f800000u | (fraction << 13u);
        } else {
            bits = sign | ((exponent - 15u + 127u) << 23u) | (fraction << 13u);
        }

        union {
            uint32_t bits;
            float number;
        } decoded = {.bits = bits};
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT32,
            .float32_value = decoded.number
        };
        return HONCH_OK;
    }

    if (initial == 0xfau) {
        if (reader->length - reader->offset < 5u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        reader->offset++;
        uint32_t bits = ((uint32_t)reader->data[reader->offset] << 24u) |
            ((uint32_t)reader->data[reader->offset + 1u] << 16u) |
            ((uint32_t)reader->data[reader->offset + 2u] << 8u) |
            (uint32_t)reader->data[reader->offset + 3u];
        reader->offset += 4u;
        union {
            uint32_t bits;
            float number;
        } decoded = {.bits = bits};
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT32,
            .float32_value = decoded.number
        };
        return HONCH_OK;
    }

    if (initial == 0xfbu) {
        if (reader->length - reader->offset < 9u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        reader->offset++;
        uint64_t bits = ((uint64_t)reader->data[reader->offset] << 56u) |
            ((uint64_t)reader->data[reader->offset + 1u] << 48u) |
            ((uint64_t)reader->data[reader->offset + 2u] << 40u) |
            ((uint64_t)reader->data[reader->offset + 3u] << 32u) |
            ((uint64_t)reader->data[reader->offset + 4u] << 24u) |
            ((uint64_t)reader->data[reader->offset + 5u] << 16u) |
            ((uint64_t)reader->data[reader->offset + 6u] << 8u) |
            (uint64_t)reader->data[reader->offset + 7u];
        reader->offset += 8u;
        union {
            uint64_t bits;
            double number;
        } decoded = {.bits = bits};
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64,
            .float64_value = decoded.number
        };
        return HONCH_OK;
    }

    if (initial == 0xf4u || initial == 0xf5u) {
        reader->offset++;
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_BOOL,
            .bool_value = initial == 0xf5u
        };
        return HONCH_OK;
    }

    if (initial == 0xf6u) {
        reader->offset++;
        *value = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_NULL
        };
        return HONCH_OK;
    }

    return HONCH_ERROR_INVALID_ARGUMENT;
}

static void honch_wire_v2_parsed_event_free(honch_wire_v2_parsed_event_t *parsed)
{
    if (parsed == NULL) {
        return;
    }

    free(parsed->event_name);
    free(parsed->distinct_id);
    free(parsed->session_id);
    for (size_t i = 0u; i < parsed->property_count; i++) {
        free((void *)parsed->properties[i].key);
        honch_wire_v2_value_free(&parsed->properties[i].value);
    }
    memset(parsed, 0, sizeof(*parsed));
}

static honch_status_t honch_parse_queued_cbor_properties(
    honch_cbor_event_reader_t *reader,
    honch_wire_v2_parsed_event_t *parsed)
{
    uint64_t property_count = 0u;
    honch_status_t status = honch_cbor_event_read_type(reader, 0xa0u, &property_count);
    if (status != HONCH_OK || property_count > HONCH_WIRE_V2_MAX_PARSED_PROPERTIES) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    for (uint64_t i = 0u; i < property_count; i++) {
        char *key = NULL;
        status = honch_cbor_event_read_text_copy(reader, &key, HONCH_MAX_EVENT_NAME, false);
        if (status != HONCH_OK) {
            return status;
        }

        honch_wire_v2_property_t property = {
            .key = key
        };
        status = honch_parse_queued_cbor_property_value(reader, &property.value, 0u);
        if (status != HONCH_OK) {
            free(key);
            return status;
        }

        if (strcmp(key, "$session_id") == 0 &&
            property.value.type == HONCH_WIRE_V2_VALUE_TYPE_STRING &&
            parsed->session_id == NULL) {
            parsed->session_id = honch_strdup(property.value.string_value);
            if (parsed->session_id == NULL) {
                free(key);
                honch_wire_v2_value_free(&property.value);
                return HONCH_ERROR_OUT_OF_MEMORY;
            }
        }

        if (honch_wire_property_is_promoted_context(key)) {
            free(key);
            honch_wire_v2_value_free(&property.value);
            continue;
        }
        for (size_t j = 0u; j < parsed->property_count; j++) {
            if (strcmp(parsed->properties[j].key, key) == 0) {
                free(key);
                honch_wire_v2_value_free(&property.value);
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
        }

        parsed->properties[parsed->property_count++] = property;
    }

    return HONCH_OK;
}

static honch_status_t honch_parse_queued_cbor_event_for_wire(
    const honch_payload_t *event,
    honch_wire_v2_parsed_event_t *parsed)
{
    memset(parsed, 0, sizeof(*parsed));

    honch_cbor_event_reader_t reader = {
        .data = event->data,
        .length = event->length,
        .offset = 0u
    };

    uint64_t map_count = 0u;
    honch_status_t status = honch_cbor_event_read_type(&reader, 0xa0u, &map_count);
    if (status != HONCH_OK) {
        return status;
    }

    for (uint64_t i = 0u; i < map_count; i++) {
        const char *key = NULL;
        size_t key_length = 0u;
        status = honch_cbor_event_read_text(&reader, &key, &key_length);
        if (status != HONCH_OK) {
            return status;
        }

        if (honch_text_equals(key, key_length, "event")) {
            status = honch_cbor_event_read_text_copy(&reader, &parsed->event_name, HONCH_MAX_EVENT_NAME, false);
        } else if (honch_text_equals(key, key_length, "distinct_id")) {
            status = honch_cbor_event_read_text_copy(&reader, &parsed->distinct_id, HONCH_MAX_DISTINCT_ID, false);
        } else if (honch_text_equals(key, key_length, "timestamp")) {
            status = honch_cbor_event_read_int(&reader, &parsed->timestamp_ms);
        } else if (honch_text_equals(key, key_length, "properties")) {
            status = honch_parse_queued_cbor_properties(&reader, parsed);
        } else {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (status != HONCH_OK) {
            return status;
        }
    }

    if (parsed->event_name == NULL || parsed->distinct_id == NULL || parsed->timestamp_ms == 0u ||
        reader.offset != reader.length) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return HONCH_OK;
}

static honch_status_t honch_core_build_wire_v2_message(
    honch_client_t *client,
    const honch_payload_t *events,
    size_t event_count,
    honch_payload_t *message,
    size_t *encoded_event_count)
{
    message->data = NULL;
    message->length = 0u;

    if (event_count == 0u || encoded_event_count == NULL ||
        client->device_id == NULL || client->device_model == NULL ||
        client->firmware_version == NULL || client->sdk_platform == NULL || client->environment == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *encoded_event_count = 0u;

    honch_wire_v2_parsed_event_t *parsed = (honch_wire_v2_parsed_event_t *)calloc(event_count, sizeof(*parsed));
    if (parsed == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_wire_v2_event_t *compact_events = (honch_wire_v2_event_t *)calloc(event_count, sizeof(*compact_events));
    if (compact_events == NULL) {
        free(parsed);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_status_t status = HONCH_OK;
    size_t parsed_count = 0u;
    for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
        status = honch_parse_queued_cbor_event_for_wire(&events[i], &parsed[i]);
        if (status != HONCH_OK) {
            *encoded_event_count = i;
            break;
        }
        if (status == HONCH_OK && i > 0u && strcmp(parsed[i].distinct_id, parsed[0].distinct_id) != 0) {
            break;
        }
        if (status == HONCH_OK && i > 0u &&
            ((parsed[i].session_id == NULL) != (parsed[0].session_id == NULL) ||
                (parsed[i].session_id != NULL && strcmp(parsed[i].session_id, parsed[0].session_id) != 0))) {
            break;
        }
        if (status == HONCH_OK) {
            compact_events[i] = (honch_wire_v2_event_t) {
                .event_name = parsed[i].event_name,
                .timestamp_ms = parsed[i].timestamp_ms,
                .properties = parsed[i].properties,
                .property_count = parsed[i].property_count
            };
            parsed_count = i + 1u;
        }
    }
    if (status != HONCH_OK || parsed_count == 0u) {
        for (size_t i = 0u; i < event_count; i++) {
            honch_wire_v2_parsed_event_free(&parsed[i]);
        }
        free(compact_events);
        free(parsed);
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }

    honch_wire_v2_batch_context_t context = {
        .distinct_id = parsed[0].distinct_id,
        .device_id = client->device_id,
        .device_model = client->device_model,
        .firmware_version = client->firmware_version,
        .sdk_platform = client->sdk_platform,
        .sdk_version = HONCH_SDK_VERSION,
        .environment = client->environment,
        .session_id = parsed[0].session_id != NULL ? parsed[0].session_id : client->session_id
    };

    uint8_t *buffer = (uint8_t *)malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (buffer == NULL) {
        for (size_t i = 0u; i < event_count; i++) {
            honch_wire_v2_parsed_event_free(&parsed[i]);
        }
        free(compact_events);
        free(parsed);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    size_t message_size = 0u;
    size_t encoded_count = 0u;
    status = HONCH_ERROR_OUT_OF_MEMORY;
    for (size_t try_count = parsed_count; try_count > 0u; try_count--) {
        status = honch_wire_v2_encode_event_batch(
            &context,
            parsed[0].timestamp_ms,
            compact_events,
            try_count,
            buffer,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &message_size);
        if (status == HONCH_OK) {
            encoded_count = try_count;
            break;
        }
        if (status != HONCH_ERROR_OUT_OF_MEMORY) {
            break;
        }
        if (try_count == 1u) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
            break;
        }
    }
    for (size_t i = 0u; i < event_count; i++) {
        honch_wire_v2_parsed_event_free(&parsed[i]);
    }
    free(compact_events);
    free(parsed);
    if (status != HONCH_OK) {
        free(buffer);
        return status;
    }

    message->data = buffer;
    message->length = message_size;
    *encoded_event_count = encoded_count;
    return HONCH_OK;
}

static honch_status_t honch_core_post_wire_v2_message(
    honch_client_t *client,
    const honch_payload_t *message,
    uint32_t message_id,
    const char *stream_id,
    honch_transport_result_t *result)
{
    if (client == NULL || message == NULL || result == NULL ||
        client->transport == NULL || client->transport->post_chunk == NULL ||
        stream_id == NULL || stream_id[0] == '\0') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *frame = (uint8_t *)malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (frame == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    honch_wire_v2_chunker_t chunker = {0};
    honch_status_t status = honch_wire_v2_chunker_begin(
        &chunker,
        message_id,
        message->data,
        message->length,
        HONCH_WIRE_V2_SOURCE_EVENTS,
        HONCH_WIRE_V2_MAX_FRAME_BYTES);
    if (status != HONCH_OK) {
        free(frame);
        return status;
    }

    *result = HONCH_TRANSPORT_RETRY;
    bool complete = false;
    while (!complete) {
        size_t frame_size = 0u;
        status = honch_wire_v2_chunker_next(
            &chunker,
            frame,
            HONCH_WIRE_V2_MAX_FRAME_BYTES,
            &frame_size,
            &complete);
        if (status != HONCH_OK) {
            free(frame);
            return status;
        }
        status = client->transport->post_chunk(
            client->transport->ctx,
            client->endpoint_url,
            client->api_key,
            stream_id,
            frame,
            frame_size,
            result);
        if (status != HONCH_OK) {
            free(frame);
            return status;
        }
        if (complete) {
            if (*result == HONCH_TRANSPORT_ACCEPTED) {
                break;
            }
            if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
                free(frame);
                return HONCH_OK;
            }
            *result = HONCH_TRANSPORT_RETRY;
            free(frame);
            return HONCH_ERROR_TRANSPORT;
        }
        if (*result == HONCH_TRANSPORT_CHUNK_STORED) {
            continue;
        }
        if (*result == HONCH_TRANSPORT_REJECTED || *result == HONCH_TRANSPORT_AUTH_ERROR) {
            free(frame);
            return HONCH_OK;
        }
        *result = HONCH_TRANSPORT_RETRY;
        free(frame);
        return HONCH_ERROR_TRANSPORT;
    }

    free(frame);
    return HONCH_OK;
}

static honch_status_t honch_core_flush_one(honch_client_t *client, bool *progressed)
{
    *progressed = false;

    size_t batch_size = client->batch_size == 0u ? 1u : client->batch_size;
    if (batch_size > HONCH_MAX_BATCH_SIZE) {
        batch_size = HONCH_MAX_BATCH_SIZE;
    }
    honch_payload_t *events = (honch_payload_t *)calloc(batch_size, sizeof(*events));
    uint64_t *sequences = (uint64_t *)calloc(batch_size, sizeof(*sequences));
    if (events == NULL || sequences == NULL) {
        free(events);
        free(sequences);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    size_t event_count = 0u;

    client->active_storage_reader_sequence = UINT64_MAX;
    honch_status_t status = HONCH_OK;
    while (event_count < batch_size) {
        honch_storage_reader_t reader = {0};
        status = honch_core_queue_peek(client, &reader);
        if (status == HONCH_ERROR_NOT_INITIALIZED) {
            status = HONCH_OK;
            break;
        }
        if (status != HONCH_OK) {
            break;
        }

        honch_payload_t event = {0};
        status = honch_core_read_queue_event(client, &reader, &event);
        if (status == HONCH_ERROR_INVALID_ARGUMENT) {
            honch_status_t dead_status = honch_core_queue_dead_letter(client, reader.sequence);
            if (dead_status == HONCH_OK) {
                *progressed = true;
                status = HONCH_ERROR_REJECTED;
            } else {
                status = dead_status;
            }
            break;
        }
        if (status != HONCH_OK) {
            break;
        }

        events[event_count] = event;
        sequences[event_count] = reader.sequence;
        event_count++;
    }

    if (status != HONCH_OK || event_count == 0u) {
        for (size_t i = 0u; i < event_count; i++) {
            free(events[i].data);
        }
        free(events);
        free(sequences);
        return status;
    }

    honch_payload_t compact_message = {0};
    size_t encoded_event_count = event_count;
    if (client->transport == NULL || client->transport->post_chunk == NULL) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    } else {
        status = honch_core_build_wire_v2_message(
            client,
            events,
            event_count,
            &compact_message,
            &encoded_event_count);
    }
    for (size_t i = 0u; i < event_count; i++) {
        free(events[i].data);
    }
    if (status != HONCH_OK) {
        if (status == HONCH_ERROR_INVALID_ARGUMENT && encoded_event_count < event_count) {
            honch_status_t dead_status = honch_core_queue_dead_letter(client, sequences[encoded_event_count]);
            if (dead_status == HONCH_OK) {
                *progressed = true;
                free(events);
                free(sequences);
                return HONCH_ERROR_REJECTED;
            }
            free(events);
            free(sequences);
            return dead_status;
        }
        free(events);
        free(sequences);
        return status;
    }
    event_count = encoded_event_count;

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    uint32_t message_id = client->wire_v2_message_id_seed + (uint32_t)sequences[0];
    status = honch_core_post_wire_v2_message(
        client,
        &compact_message,
        message_id,
        client->wire_v2_stream_id,
        &result);
    free(compact_message.data);

    if (result == HONCH_TRANSPORT_ACCEPTED && status == HONCH_OK) {
        for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
            status = honch_core_queue_consume(client, sequences[i]);
        }
        *progressed = status == HONCH_OK;
        free(events);
        free(sequences);
        return status;
    }

    if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
        honch_status_t dead_status = HONCH_OK;
        for (size_t i = 0u; dead_status == HONCH_OK && i < event_count; i++) {
            dead_status = honch_core_queue_dead_letter(client, sequences[i]);
        }
        if (dead_status == HONCH_OK) {
            *progressed = true;
            free(events);
            free(sequences);
            return status == HONCH_OK ? HONCH_ERROR_REJECTED : status;
        }
        free(events);
        free(sequences);
        return dead_status;
    }

    free(events);
    free(sequences);
    return status == HONCH_OK ? HONCH_ERROR_TRANSPORT : status;
}

honch_status_t honch_queue_flush_locked(honch_client_t *client)
{
    honch_status_t final_status = HONCH_OK;

    for (;;) {
        size_t depth = 0u;
        honch_status_t status = honch_core_queue_depth(client, &depth);
        if (status != HONCH_OK) {
            return status;
        }
        client->queued_event_count = depth;
        if (depth == 0u) {
            return final_status;
        }

        bool progressed = false;
        status = honch_core_flush_one(client, &progressed);
        if (status == HONCH_ERROR_REJECTED && progressed) {
            final_status = HONCH_ERROR_REJECTED;
            continue;
        }
        if (status != HONCH_OK) {
            return status;
        }
    }
}
