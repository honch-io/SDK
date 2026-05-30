#ifndef HONCH_CORE_WIRE_V2_H
#define HONCH_CORE_WIRE_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HONCH_WIRE_V2_PROTOCOL_VERSION 2u

typedef enum honch_wire_v2_source_type {
    HONCH_WIRE_V2_SOURCE_EVENTS = 0u
} honch_wire_v2_source_type_t;

typedef struct honch_wire_v2_context_extension honch_wire_v2_context_extension_t;

typedef struct honch_wire_v2_frame_spec {
    uint64_t message_id;
    size_t total_message_length;
    size_t offset;
    const uint8_t *payload;
    size_t payload_size;
    const uint8_t *crc_message;
    size_t crc_message_size;
    honch_wire_v2_source_type_t source_type;
    bool continuation;
    bool more;
} honch_wire_v2_frame_spec_t;

typedef struct honch_wire_v2_chunker {
    const uint8_t *message;
    size_t message_size;
    size_t offset;
    size_t max_frame_size;
    uint64_t message_id;
    honch_wire_v2_source_type_t source_type;
    bool active;
} honch_wire_v2_chunker_t;

typedef struct honch_wire_v2_batch_context {
    const char *distinct_id;
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *sdk_platform;
    const char *sdk_version;
    const char *environment;
    const char *session_id;
    bool has_device_time_source;
    uint64_t device_time_source;
    bool has_capabilities;
    uint64_t capabilities;
    const honch_wire_v2_context_extension_t *extensions;
    size_t extension_count;
} honch_wire_v2_batch_context_t;

typedef enum honch_wire_v2_value_type {
    HONCH_WIRE_V2_VALUE_TYPE_NULL = 0,
    HONCH_WIRE_V2_VALUE_TYPE_BOOL = 1,
    HONCH_WIRE_V2_VALUE_TYPE_UINT = 2,
    HONCH_WIRE_V2_VALUE_TYPE_INT = 3,
    HONCH_WIRE_V2_VALUE_TYPE_FLOAT32 = 4,
    HONCH_WIRE_V2_VALUE_TYPE_FLOAT64 = 5,
    HONCH_WIRE_V2_VALUE_TYPE_STRING = 6,
    HONCH_WIRE_V2_VALUE_TYPE_BYTES = 7,
    HONCH_WIRE_V2_VALUE_TYPE_ARRAY = 8,
    HONCH_WIRE_V2_VALUE_TYPE_MAP = 9
} honch_wire_v2_value_type_t;

typedef struct honch_wire_v2_value honch_wire_v2_value_t;
typedef struct honch_wire_v2_map_pair honch_wire_v2_map_pair_t;

typedef struct honch_wire_v2_bytes {
    const uint8_t *data;
    size_t size;
} honch_wire_v2_bytes_t;

typedef struct honch_wire_v2_array {
    const honch_wire_v2_value_t *items;
    size_t count;
} honch_wire_v2_array_t;

typedef struct honch_wire_v2_map {
    const honch_wire_v2_map_pair_t *entries;
    size_t count;
} honch_wire_v2_map_t;

struct honch_wire_v2_value {
    honch_wire_v2_value_type_t type;
    union {
        bool bool_value;
        uint64_t uint_value;
        int64_t int_value;
        float float32_value;
        double float64_value;
        const char *string_value;
        honch_wire_v2_bytes_t bytes;
        honch_wire_v2_array_t array;
        honch_wire_v2_map_t map;
    };
    /* For string values, SIZE_MAX means NUL-terminated C string.
       Any other value is an exact UTF-8 byte length; 0 encodes empty. */
    size_t string_size;
};

struct honch_wire_v2_map_pair {
    const char *key;
    honch_wire_v2_value_t value;
};

struct honch_wire_v2_context_extension {
    uint64_t key_id;
    honch_wire_v2_value_t value;
};

typedef struct honch_wire_v2_property {
    const char *key;
    honch_wire_v2_value_t value;
} honch_wire_v2_property_t;

typedef struct honch_wire_v2_event {
    const char *event_name;
    uint64_t timestamp_ms;
    const honch_wire_v2_property_t *properties;
    size_t property_count;
} honch_wire_v2_event_t;

static inline honch_wire_v2_value_t honch_null(void)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_NULL;
    return out;
}

static inline honch_wire_v2_value_t honch_bool(bool value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_BOOL;
    out.bool_value = value;
    return out;
}

static inline honch_wire_v2_value_t honch_u64(uint64_t value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_UINT;
    out.uint_value = value;
    return out;
}

static inline honch_wire_v2_value_t honch_i64(int64_t value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_INT;
    out.int_value = value;
    return out;
}

static inline honch_wire_v2_value_t honch_f32(float value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT32;
    out.float32_value = value;
    return out;
}

static inline honch_wire_v2_value_t honch_f64(double value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64;
    out.float64_value = value;
    return out;
}

static inline honch_wire_v2_value_t honch_str(const char *value)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_STRING;
    out.string_value = value;
    out.string_size = SIZE_MAX;
    return out;
}

static inline honch_wire_v2_value_t honch_strn(const char *value, size_t size)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_STRING;
    out.string_value = value;
    out.string_size = size;
    return out;
}

static inline honch_wire_v2_value_t honch_bytes(const uint8_t *data, size_t size)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_BYTES;
    out.bytes.data = data;
    out.bytes.size = size;
    return out;
}

static inline honch_wire_v2_value_t honch_array(const honch_wire_v2_value_t *items, size_t count)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY;
    out.array.items = items;
    out.array.count = count;
    return out;
}

static inline honch_wire_v2_value_t honch_map(const honch_wire_v2_map_pair_t *entries, size_t count)
{
    honch_wire_v2_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = HONCH_WIRE_V2_VALUE_TYPE_MAP;
    out.map.entries = entries;
    out.map.count = count;
    return out;
}

static inline honch_wire_v2_property_t honch_prop(const char *key, honch_wire_v2_value_t value)
{
    honch_wire_v2_property_t out;
    out.key = key;
    out.value = value;
    return out;
}

static inline honch_wire_v2_map_pair_t honch_pair(const char *key, honch_wire_v2_value_t value)
{
    honch_wire_v2_map_pair_t out;
    out.key = key;
    out.value = value;
    return out;
}

uint64_t honch_wire_v2_zigzag_i64(int64_t value);
uint16_t honch_wire_v2_crc16(const uint8_t *data, size_t data_size);
honch_status_t honch_wire_v2_encode_uvarint(uint64_t value, uint8_t *out, size_t out_size, size_t *written);
honch_status_t honch_wire_v2_encode_svarint(int64_t value, uint8_t *out, size_t out_size, size_t *written);
int64_t honch_wire_v2_unzigzag_i64(uint64_t value);
honch_status_t honch_wire_v2_encode_frame(
    const honch_wire_v2_frame_spec_t *spec,
    uint8_t *out,
    size_t out_size,
    size_t *written);
honch_status_t honch_wire_v2_chunker_begin(
    honch_wire_v2_chunker_t *chunker,
    uint64_t message_id,
    const uint8_t *message,
    size_t message_size,
    honch_wire_v2_source_type_t source_type,
    size_t max_frame_size);
honch_status_t honch_wire_v2_chunker_next(
    honch_wire_v2_chunker_t *chunker,
    uint8_t *out,
    size_t out_size,
    size_t *written,
    bool *message_complete);
honch_status_t honch_wire_v2_encode_event_batch(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    const honch_wire_v2_event_t *events,
    size_t event_count,
    uint8_t *out,
    size_t out_size,
    size_t *written);

#ifdef __cplusplus
}
#endif

#endif
