#include "honch/core/wire_v2.h"

#include "honch_internal.h"

#include <float.h>
#include <limits.h>
#include <string.h>

#define HONCH_WIRE_V2_HEADER_CONTINUATION 0x20u
#define HONCH_WIRE_V2_HEADER_MORE 0x40u
#define HONCH_WIRE_V2_FINAL_CRC_SIZE 2u
#define HONCH_WIRE_V2_MESSAGE_KIND_EVENT_BATCH 1u
#define HONCH_WIRE_V2_FLAG_CONTEXT_PROMOTES_PROPERTIES 1u
#define HONCH_WIRE_V2_MAX_SDK_MESSAGE_BYTES 4096u
#define HONCH_WIRE_V2_VALUE_NULL 0x00u
#define HONCH_WIRE_V2_VALUE_FALSE 0x01u
#define HONCH_WIRE_V2_VALUE_TRUE 0x02u
#define HONCH_WIRE_V2_VALUE_UINT 0x03u
#define HONCH_WIRE_V2_VALUE_INT 0x04u
#define HONCH_WIRE_V2_VALUE_FLOAT32 0x05u
#define HONCH_WIRE_V2_VALUE_FLOAT64 0x06u
#define HONCH_WIRE_V2_VALUE_STRING_REF 0x07u
#define HONCH_WIRE_V2_VALUE_BYTES 0x08u
#define HONCH_WIRE_V2_VALUE_ARRAY 0x09u
#define HONCH_WIRE_V2_VALUE_MAP 0x0au
#define HONCH_WIRE_V2_MAX_SDK_EVENTS 50u
#define HONCH_WIRE_V2_MAX_SDK_STRINGS 128u
#define HONCH_WIRE_V2_MAX_SDK_STRING_BYTES 512u
#define HONCH_WIRE_V2_MAX_SDK_TOTAL_STRING_BYTES 2048u
#define HONCH_WIRE_V2_MAX_SDK_PROPERTIES 64u
#define HONCH_WIRE_V2_MAX_NESTED_DEPTH 8u

uint64_t honch_wire_v2_zigzag_i64(int64_t value)
{
    uint64_t bits = (uint64_t)value;
    return (bits << 1u) ^ (uint64_t)(value >> 63);
}

uint16_t honch_wire_v2_crc16(const uint8_t *data, size_t data_size)
{
    uint16_t crc = 0xffffu;
    for (size_t i = 0u; i < data_size; i++) {
        crc ^= (uint16_t)data[i] << 8u;
        for (size_t bit = 0u; bit < 8u; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }
    return crc;
}

honch_status_t honch_wire_v2_encode_uvarint(uint64_t value, uint8_t *out, size_t out_size, size_t *written)
{
    if (out == NULL || written == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t used = 0u;
    do {
        if (used >= out_size || used >= 10u) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }

        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        if (value != 0u) {
            byte |= 0x80u;
        }
        out[used++] = byte;
    } while (value != 0u);

    *written = used;
    return HONCH_OK;
}

typedef struct honch_wire_v2_string_table {
    const char *items[HONCH_WIRE_V2_MAX_SDK_STRINGS];
    size_t sizes[HONCH_WIRE_V2_MAX_SDK_STRINGS];
    size_t total_bytes;
    size_t count;
} honch_wire_v2_string_table_t;

static const char s_honch_wire_v2_empty_string[] = "";

static honch_status_t honch_wire_v2_append_uvarint(
    uint64_t value,
    uint8_t *out,
    size_t out_size,
    size_t *offset);
static honch_status_t honch_wire_v2_reserved_property_id(const char *key, uint64_t *out);
static bool honch_wire_v2_property_key_is_promoted_context(const char *key);
static honch_status_t honch_wire_v2_append_key_value(
    const honch_wire_v2_string_table_t *table,
    const char *key,
    const honch_wire_v2_value_t *value,
    uint8_t *out,
    size_t out_size,
    size_t *offset);

static honch_status_t honch_wire_v2_append_byte(uint8_t value, uint8_t *out, size_t out_size, size_t *offset)
{
    if (*offset >= out_size) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    out[(*offset)++] = value;
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_append_bytes(
    const uint8_t *value,
    size_t value_size,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    if (value_size > out_size - *offset) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    memcpy(out + *offset, value, value_size);
    *offset += value_size;
    return HONCH_OK;
}

static bool honch_wire_v2_required_string_valid(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static bool honch_wire_v2_utf8_valid(const char *value, size_t value_size)
{
    const unsigned char *bytes = (const unsigned char *)value;
    size_t i = 0u;
    while (i < value_size) {
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

        if (continuation_count > value_size - i - 1u) {
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

static honch_status_t honch_wire_v2_table_intern(
    honch_wire_v2_string_table_t *table,
    const char *value,
    size_t value_size,
    bool allow_empty,
    size_t *index)
{
    if (table == NULL || value == NULL || index == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (value_size == SIZE_MAX) {
        value_size = strlen(value);
    }
    if (!allow_empty && value_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    for (size_t i = 0u; i < table->count; i++) {
        if (table->sizes[i] == value_size &&
            (value_size == 0u || memcmp(table->items[i], value, value_size) == 0)) {
            *index = i;
            return HONCH_OK;
        }
    }

    if (!honch_wire_v2_utf8_valid(value, value_size) ||
        value_size > HONCH_WIRE_V2_MAX_SDK_STRING_BYTES ||
        value_size > HONCH_WIRE_V2_MAX_SDK_TOTAL_STRING_BYTES - table->total_bytes ||
        table->count >= HONCH_WIRE_V2_MAX_SDK_STRINGS) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *index = table->count;
    table->items[table->count] = value_size == 0u ? s_honch_wire_v2_empty_string : value;
    table->sizes[table->count] = value_size;
    table->count++;
    table->total_bytes += value_size;
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_table_find(
    const honch_wire_v2_string_table_t *table,
    const char *value,
    size_t value_size,
    size_t *index)
{
    if (table == NULL || value == NULL || index == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (value_size == SIZE_MAX) {
        value_size = strlen(value);
    }

    for (size_t i = 0u; i < table->count; i++) {
        if (table->sizes[i] == value_size &&
            (value_size == 0u || memcmp(table->items[i], value, value_size) == 0)) {
            *index = i;
            return HONCH_OK;
        }
    }
    return HONCH_ERROR_INVALID_ARGUMENT;
}

static honch_status_t honch_wire_v2_collect_key_string(
    honch_wire_v2_string_table_t *table,
    const char *key,
    size_t *ignored)
{
    if (!honch_wire_v2_required_string_valid(key) || honch_wire_v2_property_key_is_promoted_context(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t reserved_id = 0u;
    honch_status_t status = honch_wire_v2_reserved_property_id(key, &reserved_id);
    if (status == HONCH_OK) {
        return HONCH_OK;
    }
    if (status != HONCH_ERROR_NOT_INITIALIZED) {
        return status;
    }
    return honch_wire_v2_table_intern(table, key, SIZE_MAX, false, ignored);
}

static honch_status_t honch_wire_v2_collect_value_strings(
    honch_wire_v2_string_table_t *table,
    const honch_wire_v2_value_t *value,
    size_t depth,
    size_t *ignored)
{
    if (value == NULL || depth > HONCH_WIRE_V2_MAX_NESTED_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    switch (value->type) {
        case HONCH_WIRE_V2_VALUE_TYPE_NULL:
        case HONCH_WIRE_V2_VALUE_TYPE_BOOL:
        case HONCH_WIRE_V2_VALUE_TYPE_UINT:
        case HONCH_WIRE_V2_VALUE_TYPE_INT:
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT32:
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT64:
        case HONCH_WIRE_V2_VALUE_TYPE_BYTES:
            return HONCH_OK;
        case HONCH_WIRE_V2_VALUE_TYPE_STRING:
            return honch_wire_v2_table_intern(table, value->string_value, value->string_size, true, ignored);
        case HONCH_WIRE_V2_VALUE_TYPE_ARRAY:
            if (value->array.count > 0u && value->array.items == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            for (size_t i = 0u; i < value->array.count; i++) {
                honch_status_t status = honch_wire_v2_collect_value_strings(
                    table,
                    &value->array.items[i],
                    depth + 1u,
                    ignored);
                if (status != HONCH_OK) {
                    return status;
                }
            }
            return HONCH_OK;
        case HONCH_WIRE_V2_VALUE_TYPE_MAP:
            if (value->map.count > 0u && value->map.entries == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            for (size_t i = 0u; i < value->map.count; i++) {
                const honch_wire_v2_map_pair_t *entry = &value->map.entries[i];
                honch_status_t status = honch_wire_v2_collect_key_string(table, entry->key, ignored);
                if (status != HONCH_OK) {
                    return status;
                }
                for (size_t j = 0u; j < i; j++) {
                    if (strcmp(value->map.entries[j].key, entry->key) == 0) {
                        return HONCH_ERROR_INVALID_ARGUMENT;
                    }
                }
                status = honch_wire_v2_collect_value_strings(table, &entry->value, depth + 1u, ignored);
                if (status != HONCH_OK) {
                    return status;
                }
            }
            return HONCH_OK;
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

static honch_status_t honch_wire_v2_build_string_table(
    const honch_wire_v2_batch_context_t *context,
    const honch_wire_v2_event_t *events,
    size_t event_count,
    honch_wire_v2_string_table_t *table)
{
    if (context == NULL || events == NULL || table == NULL ||
        !honch_wire_v2_required_string_valid(context->distinct_id) ||
        !honch_wire_v2_required_string_valid(context->device_id) ||
        !honch_wire_v2_required_string_valid(context->device_model) ||
        !honch_wire_v2_required_string_valid(context->firmware_version) ||
        !honch_wire_v2_required_string_valid(context->sdk_platform) ||
        !honch_wire_v2_required_string_valid(context->sdk_version) ||
        !honch_wire_v2_required_string_valid(context->environment)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    const char *required_context[] = {
        context->distinct_id,
        context->device_id,
        context->device_model,
        context->firmware_version,
        context->sdk_platform,
        context->sdk_version,
        context->environment
    };

    size_t ignored = 0u;
    for (size_t i = 0u; i < sizeof(required_context) / sizeof(required_context[0]); i++) {
        honch_status_t status = honch_wire_v2_table_intern(table, required_context[i], SIZE_MAX, false, &ignored);
        if (status != HONCH_OK) {
            return status;
        }
    }
    if (context->session_id != NULL) {
        honch_status_t status = honch_wire_v2_table_intern(table, context->session_id, SIZE_MAX, false, &ignored);
        if (status != HONCH_OK) {
            return status;
        }
    }
    if (context->extension_count > 0u && context->extensions == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < context->extension_count; i++) {
        if (context->extensions[i].key_id < 128u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        for (size_t j = 0u; j < i; j++) {
            if (context->extensions[j].key_id == context->extensions[i].key_id) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
        }
        honch_status_t status = honch_wire_v2_collect_value_strings(
            table,
            &context->extensions[i].value,
            0u,
            &ignored);
        if (status != HONCH_OK) {
            return status;
        }
    }

    for (size_t i = 0u; i < event_count; i++) {
        if (!honch_wire_v2_required_string_valid(events[i].event_name) ||
            events[i].property_count > HONCH_WIRE_V2_MAX_SDK_PROPERTIES ||
            (events[i].property_count > 0u && events[i].properties == NULL)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        honch_status_t status = honch_wire_v2_table_intern(table, events[i].event_name, SIZE_MAX, false, &ignored);
        if (status != HONCH_OK) {
            return status;
        }
        for (size_t j = 0u; j < events[i].property_count; j++) {
            const honch_wire_v2_property_t *property = &events[i].properties[j];
            if (!honch_wire_v2_required_string_valid(property->key)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            for (size_t k = 0u; k < j; k++) {
                if (strcmp(events[i].properties[k].key, property->key) == 0) {
                    return HONCH_ERROR_INVALID_ARGUMENT;
                }
            }
            status = honch_wire_v2_collect_key_string(table, property->key, &ignored);
            if (status != HONCH_OK) {
                return status;
            }
            status = honch_wire_v2_collect_value_strings(table, &property->value, 0u, &ignored);
            if (status != HONCH_OK) {
                return status;
            }
        }
    }

    return HONCH_OK;
}

static honch_status_t honch_wire_v2_append_string_ref_value(
    size_t index,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_STRING_REF, out, out_size, offset);
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint((uint64_t)index, out, out_size, offset);
    }
    return status;
}

static honch_status_t honch_wire_v2_append_context_entry(
    uint64_t key_id,
    const honch_wire_v2_string_table_t *table,
    const char *value,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    size_t index = 0u;
    honch_status_t status = honch_wire_v2_table_find(table, value, SIZE_MAX, &index);
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(key_id, out, out_size, offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_string_ref_value(index, out, out_size, offset);
    }
    return status;
}

static honch_status_t honch_wire_v2_append_uint_context_entry(
    uint64_t key_id,
    uint64_t value,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    honch_status_t status = honch_wire_v2_append_uvarint(key_id, out, out_size, offset);
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_UINT, out, out_size, offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(value, out, out_size, offset);
    }
    return status;
}

static honch_status_t honch_wire_v2_time_delta(uint64_t timestamp_ms, uint64_t base_time_ms, int64_t *out)
{
    if (out == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (timestamp_ms >= base_time_ms) {
        uint64_t delta = timestamp_ms - base_time_ms;
        if (delta > (uint64_t)INT64_MAX) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        *out = (int64_t)delta;
        return HONCH_OK;
    }

    uint64_t delta = base_time_ms - timestamp_ms;
    if (delta > (uint64_t)INT64_MAX + 1u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *out = delta == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)delta;
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_reserved_property_id(const char *key, uint64_t *out)
{
    static const struct {
        const char *key;
        uint64_t id;
    } reserved[] = {
        {"$battery_level", 1u},
        {"$wifi_rssi", 2u},
        {"reset_reason", 3u},
        {"state", 4u},
        {"previous_version", 5u},
        {"new_version", 6u},
        {"session_name", 7u}
    };

    for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(key, reserved[i].key) == 0) {
            *out = reserved[i].id;
            return HONCH_OK;
        }
    }
    return HONCH_ERROR_NOT_INITIALIZED;
}

static bool honch_wire_v2_property_key_is_promoted_context(const char *key)
{
    static const char *promoted[] = {
        "distinct_id",
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

static honch_status_t honch_wire_v2_append_value(
    const honch_wire_v2_string_table_t *table,
    const honch_wire_v2_value_t *value,
    uint8_t *out,
    size_t out_size,
    size_t *offset,
    size_t depth)
{
    if (value == NULL || depth > HONCH_WIRE_V2_MAX_NESTED_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    switch (value->type) {
        case HONCH_WIRE_V2_VALUE_TYPE_NULL:
            return honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_NULL, out, out_size, offset);
        case HONCH_WIRE_V2_VALUE_TYPE_BOOL:
            return honch_wire_v2_append_byte(
                value->bool_value ? HONCH_WIRE_V2_VALUE_TRUE : HONCH_WIRE_V2_VALUE_FALSE,
                out,
                out_size,
                offset);
        case HONCH_WIRE_V2_VALUE_TYPE_UINT: {
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_UINT, out, out_size, offset);
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_uvarint(value->uint_value, out, out_size, offset);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_INT: {
            size_t used = 0u;
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_INT, out, out_size, offset);
            if (status == HONCH_OK) {
                status = honch_wire_v2_encode_svarint(value->int_value, out + *offset, out_size - *offset, &used);
            }
            if (status == HONCH_OK) {
                *offset += used;
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT32: {
            float number = value->float32_value;
            if (number != number || number > FLT_MAX || number < -FLT_MAX) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            union {
                float number;
                uint32_t bits;
            } encoded = {.number = number};
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_FLOAT32, out, out_size, offset);
            if (status == HONCH_OK) {
                for (size_t i = 0u; status == HONCH_OK && i < sizeof(encoded.bits); i++) {
                    status = honch_wire_v2_append_byte(
                        (uint8_t)(encoded.bits >> (i * 8u)),
                        out,
                        out_size,
                        offset);
                }
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_FLOAT64: {
            double number = value->float64_value;
            if (number != number || number > DBL_MAX || number < -DBL_MAX) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            union {
                double number;
                uint64_t bits;
            } encoded = {.number = number};
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_FLOAT64, out, out_size, offset);
            if (status == HONCH_OK) {
                for (size_t i = 0u; status == HONCH_OK && i < sizeof(encoded.bits); i++) {
                    status = honch_wire_v2_append_byte(
                        (uint8_t)(encoded.bits >> (i * 8u)),
                        out,
                        out_size,
                        offset);
                }
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_STRING: {
            size_t index = 0u;
            honch_status_t status = honch_wire_v2_table_find(table, value->string_value, value->string_size, &index);
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_string_ref_value(index, out, out_size, offset);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_BYTES: {
            if (value->bytes.size > 0u && value->bytes.data == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_BYTES, out, out_size, offset);
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_uvarint((uint64_t)value->bytes.size, out, out_size, offset);
            }
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_bytes(value->bytes.data, value->bytes.size, out, out_size, offset);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_ARRAY: {
            if (value->array.count > 0u && value->array.items == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_ARRAY, out, out_size, offset);
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_uvarint((uint64_t)value->array.count, out, out_size, offset);
            }
            for (size_t i = 0u; status == HONCH_OK && i < value->array.count; i++) {
                status = honch_wire_v2_append_value(
                    table,
                    &value->array.items[i],
                    out,
                    out_size,
                    offset,
                    depth + 1u);
            }
            return status;
        }
        case HONCH_WIRE_V2_VALUE_TYPE_MAP: {
            if (value->map.count > 0u && value->map.entries == NULL) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_status_t status = honch_wire_v2_append_byte(HONCH_WIRE_V2_VALUE_MAP, out, out_size, offset);
            if (status == HONCH_OK) {
                status = honch_wire_v2_append_uvarint((uint64_t)value->map.count, out, out_size, offset);
            }
            for (size_t i = 0u; status == HONCH_OK && i < value->map.count; i++) {
                status = honch_wire_v2_append_key_value(
                    table,
                    value->map.entries[i].key,
                    &value->map.entries[i].value,
                    out,
                    out_size,
                    offset);
            }
            return status;
        }
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

static honch_status_t honch_wire_v2_append_key_value(
    const honch_wire_v2_string_table_t *table,
    const char *key,
    const honch_wire_v2_value_t *value,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    if (!honch_wire_v2_required_string_valid(key) ||
        honch_wire_v2_property_key_is_promoted_context(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t reserved_id = 0u;
    honch_status_t status = honch_wire_v2_reserved_property_id(key, &reserved_id);
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(reserved_id << 1u, out, out_size, offset);
    } else if (status == HONCH_ERROR_NOT_INITIALIZED) {
        size_t key_index = 0u;
        status = honch_wire_v2_table_find(table, key, SIZE_MAX, &key_index);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_wire_v2_append_uvarint(((uint64_t)key_index << 1u) | 1u, out, out_size, offset);
    } else {
        return status;
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_value(table, value, out, out_size, offset, 0u);
    }
    return status;
}

static honch_status_t honch_wire_v2_append_property(
    const honch_wire_v2_string_table_t *table,
    const honch_wire_v2_property_t *property,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    if (property == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return honch_wire_v2_append_key_value(table, property->key, &property->value, out, out_size, offset);
}

honch_status_t honch_wire_v2_encode_event_batch(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    const honch_wire_v2_event_t *events,
    size_t event_count,
    uint8_t *out,
    size_t out_size,
    size_t *written)
{
    if (out == NULL || written == NULL || event_count == 0u || event_count > HONCH_WIRE_V2_MAX_SDK_EVENTS) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *written = 0u;

    honch_wire_v2_string_table_t table = {0};
    honch_status_t status = honch_wire_v2_build_string_table(context, events, event_count, &table);
    if (status != HONCH_OK) {
        return status;
    }

    size_t offset = 0u;
    status = honch_wire_v2_append_uvarint(HONCH_WIRE_V2_PROTOCOL_VERSION, out, out_size, &offset);
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(HONCH_WIRE_V2_MESSAGE_KIND_EVENT_BATCH, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(HONCH_WIRE_V2_FLAG_CONTEXT_PROMOTES_PROPERTIES, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint(base_time_ms, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint((uint64_t)table.count, out, out_size, &offset);
    }
    for (size_t i = 0u; status == HONCH_OK && i < table.count; i++) {
        size_t string_size = table.sizes[i];
        status = honch_wire_v2_append_uvarint((uint64_t)string_size, out, out_size, &offset);
        if (status == HONCH_OK) {
            status = honch_wire_v2_append_bytes((const uint8_t *)table.items[i], string_size, out, out_size, &offset);
        }
    }

    if (status == HONCH_OK && context->has_device_time_source && context->device_time_source > 3u) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (status == HONCH_OK) {
        uint64_t context_count = 7u;
        if (context->session_id != NULL) {
            context_count++;
        }
        if (context->has_device_time_source) {
            context_count++;
        }
        if (context->has_capabilities) {
            context_count++;
        }
        context_count += (uint64_t)context->extension_count;
        status = honch_wire_v2_append_uvarint(context_count, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(1u, &table, context->distinct_id, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(2u, &table, context->device_id, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(3u, &table, context->device_model, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(4u, &table, context->firmware_version, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(5u, &table, context->sdk_platform, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(6u, &table, context->sdk_version, out, out_size, &offset);
    }
    if (status == HONCH_OK) {
        status = honch_wire_v2_append_context_entry(7u, &table, context->environment, out, out_size, &offset);
    }
    if (status == HONCH_OK && context->session_id != NULL) {
        status = honch_wire_v2_append_context_entry(8u, &table, context->session_id, out, out_size, &offset);
    }
    if (status == HONCH_OK && context->has_device_time_source) {
        status = honch_wire_v2_append_uint_context_entry(
            9u,
            context->device_time_source,
            out,
            out_size,
            &offset);
    }
    if (status == HONCH_OK && context->has_capabilities) {
        status = honch_wire_v2_append_uint_context_entry(
            10u,
            context->capabilities,
            out,
            out_size,
            &offset);
    }
    for (size_t i = 0u; status == HONCH_OK && i < context->extension_count; i++) {
        status = honch_wire_v2_append_uvarint(context->extensions[i].key_id, out, out_size, &offset);
        if (status == HONCH_OK) {
            status = honch_wire_v2_append_value(
                &table,
                &context->extensions[i].value,
                out,
                out_size,
                &offset,
                0u);
        }
    }

    if (status == HONCH_OK) {
        status = honch_wire_v2_append_uvarint((uint64_t)event_count, out, out_size, &offset);
    }
    for (size_t i = 0u; status == HONCH_OK && i < event_count; i++) {
        size_t name_index = 0u;
        int64_t delta = 0;
        status = honch_wire_v2_table_find(&table, events[i].event_name, SIZE_MAX, &name_index);
        if (status == HONCH_OK) {
            status = honch_wire_v2_time_delta(events[i].timestamp_ms, base_time_ms, &delta);
        }
        if (status == HONCH_OK) {
            status = honch_wire_v2_append_uvarint((uint64_t)name_index, out, out_size, &offset);
        }
        if (status == HONCH_OK) {
            status = honch_wire_v2_encode_svarint(delta, out + offset, out_size - offset, written);
        }
        if (status == HONCH_OK) {
            offset += *written;
            status = honch_wire_v2_append_uvarint((uint64_t)events[i].property_count, out, out_size, &offset);
        }
        for (size_t j = 0u; status == HONCH_OK && j < events[i].property_count; j++) {
            status = honch_wire_v2_append_property(&table, &events[i].properties[j], out, out_size, &offset);
        }
    }

    if (status == HONCH_OK && offset > HONCH_WIRE_V2_MAX_SDK_MESSAGE_BYTES) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (status != HONCH_OK) {
        *written = 0u;
        return status;
    }

    *written = offset;
    return HONCH_OK;
}

honch_status_t honch_wire_v2_encode_svarint(int64_t value, uint8_t *out, size_t out_size, size_t *written)
{
    return honch_wire_v2_encode_uvarint(honch_wire_v2_zigzag_i64(value), out, out_size, written);
}

static honch_status_t honch_wire_v2_uvarint_size(uint64_t value, size_t *out)
{
    uint8_t buffer[10] = {0};
    return honch_wire_v2_encode_uvarint(value, buffer, sizeof(buffer), out);
}

static honch_status_t honch_wire_v2_append_uvarint(
    uint64_t value,
    uint8_t *out,
    size_t out_size,
    size_t *offset)
{
    size_t used = 0u;
    honch_status_t status = honch_wire_v2_encode_uvarint(value, out + *offset, out_size - *offset, &used);
    if (status != HONCH_OK) {
        return status;
    }
    *offset += used;
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_check_frame_spec(const honch_wire_v2_frame_spec_t *spec)
{
    if (spec == NULL || spec->payload == NULL ||
        (spec->source_type != HONCH_WIRE_V2_SOURCE_EVENTS) ||
        spec->payload_size > spec->total_message_length ||
        spec->offset > spec->total_message_length ||
        spec->payload_size > spec->total_message_length - spec->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (!spec->more) {
        if (spec->payload_size != spec->total_message_length - spec->offset) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (spec->crc_message != NULL) {
            if (spec->crc_message_size != spec->total_message_length) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
        } else if (spec->offset != 0u || spec->payload_size != spec->total_message_length) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
    } else if (spec->payload_size == spec->total_message_length - spec->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (spec->continuation && spec->offset == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (!spec->continuation && spec->offset != 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

honch_status_t honch_wire_v2_encode_frame(
    const honch_wire_v2_frame_spec_t *spec,
    uint8_t *out,
    size_t out_size,
    size_t *written)
{
    if (out == NULL || written == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *written = 0u;

    honch_status_t status = honch_wire_v2_check_frame_spec(spec);
    if (status != HONCH_OK) {
        return status;
    }

    size_t fixed_size = 1u;
    status = honch_size_add(fixed_size, spec->payload_size, &fixed_size);
    if (status == HONCH_OK && !spec->more) {
        status = honch_size_add(fixed_size, HONCH_WIRE_V2_FINAL_CRC_SIZE, &fixed_size);
    }
    if (status != HONCH_OK || out_size < fixed_size) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    uint8_t header = HONCH_WIRE_V2_PROTOCOL_VERSION;
    header |= (uint8_t)((uint8_t)spec->source_type << 2u);
    if (spec->continuation) {
        header |= HONCH_WIRE_V2_HEADER_CONTINUATION;
    }
    if (spec->more) {
        header |= HONCH_WIRE_V2_HEADER_MORE;
    }

    size_t offset = 0u;
    out[offset++] = header;
    status = honch_wire_v2_append_uvarint(spec->message_id, out, out_size, &offset);
    if (status == HONCH_OK && spec->continuation) {
        status = honch_wire_v2_append_uvarint((uint64_t)spec->offset, out, out_size, &offset);
    } else if (status == HONCH_OK && spec->more) {
        status = honch_wire_v2_append_uvarint((uint64_t)spec->total_message_length, out, out_size, &offset);
    }
    if (status != HONCH_OK) {
        return status;
    }

    size_t remaining_size = spec->payload_size + (spec->more ? 0u : HONCH_WIRE_V2_FINAL_CRC_SIZE);
    if (remaining_size > out_size - offset) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    memcpy(out + offset, spec->payload, spec->payload_size);
    offset += spec->payload_size;

    if (!spec->more) {
        const uint8_t *crc_data = spec->crc_message == NULL ? spec->payload : spec->crc_message;
        size_t crc_size = spec->crc_message == NULL ? spec->payload_size : spec->crc_message_size;
        uint16_t crc = honch_wire_v2_crc16(crc_data, crc_size);
        out[offset++] = (uint8_t)crc;
        out[offset++] = (uint8_t)(crc >> 8u);
    }

    *written = offset;
    return HONCH_OK;
}

honch_status_t honch_wire_v2_chunker_begin(
    honch_wire_v2_chunker_t *chunker,
    uint64_t message_id,
    const uint8_t *message,
    size_t message_size,
    honch_wire_v2_source_type_t source_type,
    size_t max_frame_size)
{
    if (chunker == NULL || message == NULL || message_size == 0u ||
        source_type != HONCH_WIRE_V2_SOURCE_EVENTS || max_frame_size < 4u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *chunker = (honch_wire_v2_chunker_t) {
        .message = message,
        .message_size = message_size,
        .offset = 0u,
        .max_frame_size = max_frame_size,
        .message_id = message_id,
        .source_type = source_type,
        .active = true
    };
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_chunker_overhead(
    const honch_wire_v2_chunker_t *chunker,
    bool continuation,
    bool more,
    size_t *out)
{
    size_t message_id_size = 0u;
    size_t position_size = 0u;
    honch_status_t status = honch_wire_v2_uvarint_size(chunker->message_id, &message_id_size);
    if (status != HONCH_OK) {
        return status;
    }

    if (continuation) {
        status = honch_wire_v2_uvarint_size((uint64_t)chunker->offset, &position_size);
    } else if (more) {
        status = honch_wire_v2_uvarint_size((uint64_t)chunker->message_size, &position_size);
    }
    if (status != HONCH_OK) {
        return status;
    }

    size_t overhead = 1u;
    status = honch_size_add(overhead, message_id_size, &overhead);
    if (status == HONCH_OK) {
        status = honch_size_add(overhead, position_size, &overhead);
    }
    if (status == HONCH_OK && !more) {
        status = honch_size_add(overhead, HONCH_WIRE_V2_FINAL_CRC_SIZE, &overhead);
    }
    if (status != HONCH_OK) {
        return status;
    }

    *out = overhead;
    return HONCH_OK;
}

static honch_status_t honch_wire_v2_chunker_payload_capacity(
    const honch_wire_v2_chunker_t *chunker,
    bool continuation,
    bool more,
    size_t *out)
{
    size_t overhead = 0u;
    honch_status_t status = honch_wire_v2_chunker_overhead(chunker, continuation, more, &overhead);
    if (status != HONCH_OK) {
        return status;
    }
    if (overhead >= chunker->max_frame_size) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *out = chunker->max_frame_size - overhead;
    return HONCH_OK;
}

honch_status_t honch_wire_v2_chunker_next(
    honch_wire_v2_chunker_t *chunker,
    uint8_t *out,
    size_t out_size,
    size_t *written,
    bool *message_complete)
{
    if (chunker == NULL || out == NULL || written == NULL || message_complete == NULL ||
        !chunker->active || chunker->offset >= chunker->message_size ||
        out_size < chunker->max_frame_size) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *written = 0u;
    *message_complete = false;

    bool continuation = chunker->offset != 0u;
    size_t remaining = chunker->message_size - chunker->offset;
    size_t final_capacity = 0u;
    honch_status_t status = honch_wire_v2_chunker_payload_capacity(chunker, continuation, false, &final_capacity);
    if (status != HONCH_OK) {
        return status;
    }

    bool more = remaining > final_capacity;
    size_t payload_capacity = final_capacity;
    if (more) {
        status = honch_wire_v2_chunker_payload_capacity(chunker, continuation, true, &payload_capacity);
        if (status != HONCH_OK) {
            return status;
        }
    }

    size_t payload_size = remaining < payload_capacity ? remaining : payload_capacity;
    if (more && payload_size >= remaining) {
        payload_size = remaining - final_capacity;
    }
    if (payload_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_wire_v2_frame_spec_t spec = {
        .message_id = chunker->message_id,
        .total_message_length = chunker->message_size,
        .offset = chunker->offset,
        .payload = chunker->message + chunker->offset,
        .payload_size = payload_size,
        .crc_message = chunker->message,
        .crc_message_size = chunker->message_size,
        .source_type = chunker->source_type,
        .continuation = continuation,
        .more = more
    };

    status = honch_wire_v2_encode_frame(&spec, out, out_size, written);
    if (status != HONCH_OK) {
        return status;
    }

    chunker->offset += payload_size;
    *message_complete = chunker->offset == chunker->message_size;
    if (*message_complete) {
        chunker->active = false;
    }
    return HONCH_OK;
}
