#include "honch_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_EVENT_RECORD_MAX_DEPTH 8u

enum {
    HONCH_RECORD_NULL = 0u,
    HONCH_RECORD_FALSE = 1u,
    HONCH_RECORD_TRUE = 2u,
    HONCH_RECORD_UINT = 3u,
    HONCH_RECORD_INT = 4u,
    HONCH_RECORD_FLOAT32 = 5u,
    HONCH_RECORD_FLOAT64 = 6u,
    HONCH_RECORD_STRING = 7u,
    HONCH_RECORD_BYTES = 8u,
    HONCH_RECORD_ARRAY = 9u,
    HONCH_RECORD_MAP = 10u
};

typedef struct honch_record_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
} honch_record_reader_t;

static const uint8_t HONCH_EVENT_RECORD_MAGIC[] = {'H', 'Q', 'R', '1'};

static honch_status_t honch_record_append_byte(honch_buffer_t *buffer, uint8_t value)
{
    return honch_buffer_append_n(buffer, (const char *)&value, 1u);
}

static honch_status_t honch_record_append_uvarint(honch_buffer_t *buffer, uint64_t value)
{
    uint8_t encoded[10];
    size_t used = 0u;
    honch_status_t status = honch_wire_v2_encode_uvarint(value, encoded, sizeof(encoded), &used);
    if (status != HONCH_OK) {
        return status;
    }
    return honch_buffer_append_n(buffer, (const char *)encoded, used);
}

static honch_status_t honch_record_append_string_n(honch_buffer_t *buffer, const char *value, size_t length, bool allow_empty)
{
    if (value == NULL || (!allow_empty && length == 0u) || !honch_utf8_is_valid(value, length)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_record_append_uvarint(buffer, (uint64_t)length);
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(buffer, value, length);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_byte(buffer, 0u);
    }
    return status;
}

static honch_status_t honch_record_append_string(honch_buffer_t *buffer, const char *value, bool allow_empty)
{
    return honch_record_append_string_n(buffer, value, strlen(value), allow_empty);
}

static honch_status_t honch_record_append_value(
    honch_buffer_t *buffer,
    const honch_wire_v2_value_t *value,
    size_t depth);
static honch_status_t honch_record_validate_unique_keys(
    const honch_wire_v2_map_pair_t *entries,
    size_t count);

static size_t honch_record_uvarint_size(uint64_t value)
{
    size_t used = 1u;
    while (value >= 0x80u) {
        value >>= 7u;
        used++;
    }
    return used;
}

static honch_status_t honch_record_measure_add(size_t *size, size_t amount)
{
    return honch_size_add(*size, amount, size);
}

static honch_status_t honch_record_measure_string_n(
    const char *value,
    size_t length,
    bool allow_empty,
    size_t *size)
{
    if (value == NULL || (!allow_empty && length == 0u) || !honch_utf8_is_valid(value, length)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    size_t encoded_length = 0u;
    honch_status_t status = honch_size_add(honch_record_uvarint_size((uint64_t)length), length, &encoded_length);
    if (status == HONCH_OK) {
        status = honch_size_add(encoded_length, 1u, &encoded_length);
    }
    if (status == HONCH_OK) {
        status = honch_record_measure_add(size, encoded_length);
    }
    return status;
}

static honch_status_t honch_record_measure_string(const char *value, bool allow_empty, size_t *size)
{
    return honch_record_measure_string_n(value, strlen(value), allow_empty, size);
}

static honch_status_t honch_record_measure_value(
    const honch_wire_v2_value_t *value,
    size_t depth,
    size_t *size);

static honch_status_t honch_record_measure_key_value(
    const char *key,
    const honch_wire_v2_value_t *value,
    size_t depth,
    size_t *size)
{
    if (honch_is_blank(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_record_measure_string(key, false, size);
    if (status == HONCH_OK) {
        status = honch_record_measure_value(value, depth, size);
    }
    return status;
}

static honch_status_t honch_record_measure_value(
    const honch_wire_v2_value_t *value,
    size_t depth,
    size_t *size)
{
    if (value == NULL || size == NULL || depth > HONCH_EVENT_RECORD_MAX_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = HONCH_OK;
    switch (value->type) {
        case HONCH_WIRE_V2_VALUE_TYPE_NULL:
        case HONCH_WIRE_V2_VALUE_TYPE_BOOL:
            return honch_record_measure_add(size, 1u);
        case HONCH_WIRE_V2_VALUE_TYPE_UINT:
            return honch_record_measure_add(size, 1u + honch_record_uvarint_size(value->uint_value));
        case HONCH_WIRE_V2_VALUE_TYPE_INT:
            return honch_record_measure_add(
                size,
                1u + honch_record_uvarint_size(honch_wire_v2_zigzag_i64(value->int_value)));
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT32:
            if (!isfinite(value->float32_value)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            return honch_record_measure_add(size, 1u + sizeof(uint32_t));
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT64:
            if (!isfinite(value->float64_value)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            return honch_record_measure_add(size, 1u + sizeof(uint64_t));
        case HONCH_WIRE_V2_VALUE_TYPE_STRING: {
            if (value->string_value == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            size_t string_size = value->string_size == SIZE_MAX ? strlen(value->string_value) : value->string_size;
            status = honch_record_measure_add(size, 1u);
            if (status == HONCH_OK) {
                status = honch_record_measure_string_n(value->string_value, string_size, true, size);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_BYTES:
            if (value->bytes.size > 0u && value->bytes.data == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_measure_add(size, 1u + honch_record_uvarint_size((uint64_t)value->bytes.size));
            if (status == HONCH_OK) {
                status = honch_record_measure_add(size, value->bytes.size);
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_ARRAY:
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH ||
                (value->array.count > 0u && value->array.items == NULL)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_measure_add(size, 1u + honch_record_uvarint_size((uint64_t)value->array.count));
            for (size_t i = 0u; status == HONCH_OK && i < value->array.count; i++) {
                status = honch_record_measure_value(&value->array.items[i], depth + 1u, size);
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_MAP:
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH ||
                (value->map.count > 0u && value->map.entries == NULL)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_validate_unique_keys(value->map.entries, value->map.count);
            if (status == HONCH_OK) {
                status = honch_record_measure_add(size, 1u + honch_record_uvarint_size((uint64_t)value->map.count));
            }
            for (size_t i = 0u; status == HONCH_OK && i < value->map.count; i++) {
                status = honch_record_measure_key_value(
                    value->map.entries[i].key,
                    &value->map.entries[i].value,
                    depth + 1u,
                    size);
            }
            return status;
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

static honch_status_t honch_record_append_key_value(
    honch_buffer_t *buffer,
    const char *key,
    const honch_wire_v2_value_t *value,
    size_t depth)
{
    if (honch_is_blank(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_record_append_string(buffer, key, false);
    if (status == HONCH_OK) {
        status = honch_record_append_value(buffer, value, depth);
    }
    return status;
}

static honch_status_t honch_record_validate_unique_keys(
    const honch_wire_v2_map_pair_t *entries,
    size_t count)
{
    for (size_t i = 0u; i < count; i++) {
        if (honch_is_blank(entries[i].key)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        for (size_t j = i + 1u; j < count; j++) {
            if (strcmp(entries[i].key, entries[j].key) == 0) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    return HONCH_OK;
}

static honch_status_t honch_record_append_value(
    honch_buffer_t *buffer,
    const honch_wire_v2_value_t *value,
    size_t depth)
{
    if (buffer == NULL || value == NULL || depth > HONCH_EVENT_RECORD_MAX_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = HONCH_OK;
    switch (value->type) {
        case HONCH_WIRE_V2_VALUE_TYPE_NULL:
            return honch_record_append_byte(buffer, HONCH_RECORD_NULL);
        case HONCH_WIRE_V2_VALUE_TYPE_BOOL:
            return honch_record_append_byte(buffer, value->bool_value ? HONCH_RECORD_TRUE : HONCH_RECORD_FALSE);
        case HONCH_WIRE_V2_VALUE_TYPE_UINT:
            status = honch_record_append_byte(buffer, HONCH_RECORD_UINT);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, value->uint_value);
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_INT:
            status = honch_record_append_byte(buffer, HONCH_RECORD_INT);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, honch_wire_v2_zigzag_i64(value->int_value));
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT32: {
            if (!isfinite(value->float32_value)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            union {
                float number;
                uint32_t bits;
            } encoded = {.number = value->float32_value};
            status = honch_record_append_byte(buffer, HONCH_RECORD_FLOAT32);
            for (size_t i = 0u; status == HONCH_OK && i < sizeof(encoded.bits); i++) {
                status = honch_record_append_byte(buffer, (uint8_t)(encoded.bits >> (i * 8u)));
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT64: {
            if (!isfinite(value->float64_value)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            union {
                double number;
                uint64_t bits;
            } encoded = {.number = value->float64_value};
            status = honch_record_append_byte(buffer, HONCH_RECORD_FLOAT64);
            for (size_t i = 0u; status == HONCH_OK && i < sizeof(encoded.bits); i++) {
                status = honch_record_append_byte(buffer, (uint8_t)(encoded.bits >> (i * 8u)));
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_STRING: {
            if (value->string_value == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            size_t string_size = value->string_size == SIZE_MAX ? strlen(value->string_value) : value->string_size;
            status = honch_record_append_byte(buffer, HONCH_RECORD_STRING);
            if (status == HONCH_OK) {
                status = honch_record_append_string_n(buffer, value->string_value, string_size, true);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_BYTES:
            if (value->bytes.size > 0u && value->bytes.data == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_append_byte(buffer, HONCH_RECORD_BYTES);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, (uint64_t)value->bytes.size);
            }
            if (status == HONCH_OK && value->bytes.size > 0u) {
                status = honch_buffer_append_n(buffer, (const char *)value->bytes.data, value->bytes.size);
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_ARRAY:
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH ||
                (value->array.count > 0u && value->array.items == NULL)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_append_byte(buffer, HONCH_RECORD_ARRAY);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, (uint64_t)value->array.count);
            }
            for (size_t i = 0u; status == HONCH_OK && i < value->array.count; i++) {
                status = honch_record_append_value(buffer, &value->array.items[i], depth + 1u);
            }
            return status;
        case HONCH_WIRE_V2_VALUE_TYPE_MAP:
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH ||
                (value->map.count > 0u && value->map.entries == NULL)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            status = honch_record_validate_unique_keys(value->map.entries, value->map.count);
            if (status != HONCH_OK) {
                return status;
            }
            status = honch_record_append_byte(buffer, HONCH_RECORD_MAP);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, (uint64_t)value->map.count);
            }
            for (size_t i = 0u; status == HONCH_OK && i < value->map.count; i++) {
                status = honch_record_append_key_value(
                    buffer,
                    value->map.entries[i].key,
                    &value->map.entries[i].value,
                    depth + 1u);
            }
            return status;
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

honch_status_t honch_event_record_build(
    const char *event_name,
    const char *distinct_id,
    const char *session_id,
    uint64_t timestamp_ms,
    const honch_wire_v2_property_t *properties,
    size_t property_count,
    honch_payload_t *out)
{
    if (out == NULL || honch_is_blank(event_name) || honch_is_blank(distinct_id) ||
        (property_count > 0u && properties == NULL) || property_count > HONCH_MAX_EVENT_PROPERTIES) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    out->data = NULL;
    out->length = 0u;
    size_t measured_size = sizeof(HONCH_EVENT_RECORD_MAGIC);
    honch_status_t status = honch_record_measure_add(&measured_size, honch_record_uvarint_size(timestamp_ms));
    if (status == HONCH_OK) {
        status = honch_record_measure_string(distinct_id, false, &measured_size);
    }
    if (status == HONCH_OK) {
        status = honch_record_measure_add(&measured_size, 1u);
    }
    if (status == HONCH_OK && session_id != NULL) {
        status = honch_record_measure_string(session_id, false, &measured_size);
    }
    if (status == HONCH_OK) {
        status = honch_record_measure_string(event_name, false, &measured_size);
    }
    if (status == HONCH_OK) {
        status = honch_record_measure_add(&measured_size, honch_record_uvarint_size((uint64_t)property_count));
    }
    for (size_t i = 0u; status == HONCH_OK && i < property_count; i++) {
        if (honch_is_blank(properties[i].key)) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
            break;
        }
        for (size_t j = i + 1u; j < property_count; j++) {
            if (strcmp(properties[i].key, properties[j].key) == 0) {
                status = HONCH_ERROR_INVALID_ARGUMENT;
                break;
            }
        }
        if (status == HONCH_OK) {
            status = honch_record_measure_key_value(
                properties[i].key,
                &properties[i].value,
                0u,
                &measured_size);
        }
    }
    if (status != HONCH_OK) {
        return status;
    }

    size_t initial_capacity = 0u;
    status = honch_size_add(measured_size, 1u, &initial_capacity);
    if (status != HONCH_OK) {
        return status;
    }

    honch_buffer_t buffer;
    status = honch_buffer_init(&buffer, initial_capacity);
    if (status != HONCH_OK) {
        return status;
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(&buffer, (const char *)HONCH_EVENT_RECORD_MAGIC, sizeof(HONCH_EVENT_RECORD_MAGIC));
    }
    if (status == HONCH_OK) {
        status = honch_record_append_uvarint(&buffer, timestamp_ms);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_string(&buffer, distinct_id, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_byte(&buffer, session_id != NULL ? 1u : 0u);
    }
    if (status == HONCH_OK && session_id != NULL) {
        status = honch_record_append_string(&buffer, session_id, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_string(&buffer, event_name, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_uvarint(&buffer, (uint64_t)property_count);
    }
    for (size_t i = 0u; status == HONCH_OK && i < property_count; i++) {
        status = honch_record_append_key_value(&buffer, properties[i].key, &properties[i].value, 0u);
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }
    out->data = (unsigned char *)buffer.data;
    out->length = buffer.length;
    return HONCH_OK;
}

static honch_status_t honch_record_read_uvarint(honch_record_reader_t *reader, uint64_t *out)
{
    uint64_t value = 0u;
    unsigned int shift = 0u;
    for (size_t i = 0u; i < 10u; i++) {
        if (reader->offset >= reader->length) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        uint8_t byte = reader->data[reader->offset++];
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *out = value;
            return HONCH_OK;
        }
        shift += 7u;
    }
    return HONCH_ERROR_INVALID_ARGUMENT;
}

static honch_status_t honch_record_read_string(
    honch_record_reader_t *reader,
    const char **out,
    size_t *out_size,
    bool allow_empty,
    bool allow_embedded_nul)
{
    uint64_t length = 0u;
    honch_status_t status = honch_record_read_uvarint(reader, &length);
    if (status != HONCH_OK || length > SIZE_MAX || (size_t)length + 1u > reader->length - reader->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    const char *value = (const char *)reader->data + reader->offset;
    if ((!allow_empty && length == 0u) ||
        value[length] != '\0' ||
        (!allow_embedded_nul && memchr(value, '\0', (size_t)length) != NULL) ||
        !honch_utf8_is_valid(value, (size_t)length)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    reader->offset += (size_t)length + 1u;
    *out = value;
    if (out_size != NULL) {
        *out_size = (size_t)length;
    }
    return HONCH_OK;
}

static void honch_record_value_free(honch_wire_v2_value_t *value)
{
    if (value == NULL) {
        return;
    }
    if (value->type == HONCH_WIRE_V2_VALUE_TYPE_ARRAY) {
        honch_wire_v2_value_t *items = (honch_wire_v2_value_t *)value->array.items;
        for (size_t i = 0u; i < value->array.count; i++) {
            honch_record_value_free(&items[i]);
        }
        free(items);
    } else if (value->type == HONCH_WIRE_V2_VALUE_TYPE_MAP) {
        honch_wire_v2_map_pair_t *entries = (honch_wire_v2_map_pair_t *)value->map.entries;
        for (size_t i = 0u; i < value->map.count; i++) {
            honch_record_value_free(&entries[i].value);
        }
        free(entries);
    }
    memset(value, 0, sizeof(*value));
}

static honch_status_t honch_record_read_value(honch_record_reader_t *reader, honch_wire_v2_value_t *value, size_t depth)
{
    if (reader->offset >= reader->length || depth > HONCH_EVENT_RECORD_MAX_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    uint8_t tag = reader->data[reader->offset++];
    switch (tag) {
        case HONCH_RECORD_NULL:
            *value = (honch_wire_v2_value_t){.type = HONCH_WIRE_V2_VALUE_TYPE_NULL};
            return HONCH_OK;
        case HONCH_RECORD_FALSE:
        case HONCH_RECORD_TRUE:
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_BOOL,
                .bool_value = tag == HONCH_RECORD_TRUE
            };
            return HONCH_OK;
        case HONCH_RECORD_UINT: {
            uint64_t number = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &number);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){.type = HONCH_WIRE_V2_VALUE_TYPE_UINT, .uint_value = number};
            return HONCH_OK;
        }
        case HONCH_RECORD_INT: {
            uint64_t number = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &number);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_INT,
                .int_value = honch_wire_v2_unzigzag_i64(number)
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_FLOAT32: {
            if (reader->length - reader->offset < 4u) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint32_t bits = 0u;
            for (size_t i = 0u; i < 4u; i++) {
                bits |= (uint32_t)reader->data[reader->offset++] << (i * 8u);
            }
            union {
                uint32_t bits;
                float number;
            } decoded = {.bits = bits};
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT32,
                .float32_value = decoded.number
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_FLOAT64: {
            if (reader->length - reader->offset < 8u) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint64_t bits = 0u;
            for (size_t i = 0u; i < 8u; i++) {
                bits |= (uint64_t)reader->data[reader->offset++] << (i * 8u);
            }
            union {
                uint64_t bits;
                double number;
            } decoded = {.bits = bits};
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64,
                .float64_value = decoded.number
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_STRING: {
            const char *string = NULL;
            size_t string_size = 0u;
            honch_status_t status = honch_record_read_string(reader, &string, &string_size, true, true);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = string,
                .string_size = string_size
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_BYTES: {
            uint64_t bytes_size = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &bytes_size);
            if (status != HONCH_OK || bytes_size > SIZE_MAX || bytes_size > reader->length - reader->offset) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            const uint8_t *bytes = reader->data + reader->offset;
            reader->offset += (size_t)bytes_size;
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_BYTES,
                .bytes = {.data = bytes, .size = (size_t)bytes_size}
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_ARRAY: {
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint64_t count = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &count);
            size_t remaining = reader->length - reader->offset;
            if (status != HONCH_OK ||
                count > SIZE_MAX / sizeof(honch_wire_v2_value_t) ||
                count > remaining) {
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
                status = honch_record_read_value(reader, &items[i], depth + 1u);
                if (status != HONCH_OK) {
                    for (uint64_t j = 0u; j < i; j++) {
                        honch_record_value_free(&items[j]);
                    }
                    free(items);
                    return status;
                }
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
                .array = {.items = items, .count = (size_t)count}
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_MAP: {
            if (depth >= HONCH_EVENT_RECORD_MAX_DEPTH) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint64_t count = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &count);
            size_t remaining = reader->length - reader->offset;
            if (status != HONCH_OK ||
                count > SIZE_MAX / sizeof(honch_wire_v2_map_pair_t) ||
                count > remaining / 3u) {
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
                status = honch_record_read_string(reader, &entries[i].key, NULL, false, false);
                if (status == HONCH_OK) {
                    status = honch_record_read_value(reader, &entries[i].value, depth + 1u);
                }
                if (status != HONCH_OK) {
                    for (uint64_t j = 0u; j < i; j++) {
                        honch_record_value_free(&entries[j].value);
                    }
                    free(entries);
                    return status;
                }
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
                .map = {.entries = entries, .count = (size_t)count}
            };
            return HONCH_OK;
        }
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

honch_status_t honch_event_record_parse(const uint8_t *data, size_t length, honch_event_record_t *record)
{
    if (data == NULL || record == NULL || length < sizeof(HONCH_EVENT_RECORD_MAGIC) ||
        memcmp(data, HONCH_EVENT_RECORD_MAGIC, sizeof(HONCH_EVENT_RECORD_MAGIC)) != 0) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memset(record, 0, sizeof(*record));
    honch_record_reader_t reader = {
        .data = data,
        .length = length,
        .offset = sizeof(HONCH_EVENT_RECORD_MAGIC)
    };
    honch_status_t status = honch_record_read_uvarint(&reader, &record->timestamp_ms);
    if (status == HONCH_OK) {
        status = honch_record_read_string(&reader, &record->distinct_id, NULL, false, false);
    }
    if (status == HONCH_OK) {
        if (reader.offset >= reader.length || reader.data[reader.offset] > 1u) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        } else if (reader.data[reader.offset++] == 1u) {
            status = honch_record_read_string(&reader, &record->session_id, NULL, false, false);
        }
    }
    if (status == HONCH_OK) {
        status = honch_record_read_string(&reader, &record->event_name, NULL, false, false);
    }
    uint64_t property_count = 0u;
    if (status == HONCH_OK) {
        status = honch_record_read_uvarint(&reader, &property_count);
    }
    if (status != HONCH_OK || property_count > HONCH_MAX_EVENT_PROPERTIES) {
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }
    for (uint64_t i = 0u; i < property_count; i++) {
        status = honch_record_read_string(&reader, &record->properties[i].key, NULL, false, false);
        if (status == HONCH_OK) {
            status = honch_record_read_value(&reader, &record->properties[i].value, 0u);
        }
        if (status != HONCH_OK) {
            honch_event_record_free(record);
            return status;
        }
        record->property_count++;
    }
    if (reader.offset != reader.length || record->timestamp_ms == 0u) {
        honch_event_record_free(record);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

void honch_event_record_free(honch_event_record_t *record)
{
    if (record == NULL) {
        return;
    }
    for (size_t i = 0u; i < record->property_count; i++) {
        honch_record_value_free(&record->properties[i].value);
    }
    memset(record, 0, sizeof(*record));
}

static bool honch_record_property_is_promoted_context(const char *key)
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

void honch_event_record_prepare_wire_properties(honch_event_record_t *record)
{
    if (record == NULL) {
        return;
    }
    size_t write_index = 0u;
    for (size_t read_index = 0u; read_index < record->property_count; read_index++) {
        honch_wire_v2_property_t property = record->properties[read_index];
        if (honch_record_property_is_promoted_context(property.key)) {
            honch_record_value_free(&property.value);
            continue;
        }
        if (write_index != read_index) {
            record->properties[write_index] = property;
            memset(&record->properties[read_index], 0, sizeof(record->properties[read_index]));
        }
        write_index++;
    }
    record->property_count = write_index;
}

bool honch_event_record_validate(const uint8_t *data, size_t length)
{
    honch_event_record_t *record = (honch_event_record_t *)calloc(1u, sizeof(*record));
    if (record == NULL) {
        return false;
    }
    honch_status_t status = honch_event_record_parse(data, length, record);
    honch_event_record_free(record);
    free(record);
    return status == HONCH_OK;
}
