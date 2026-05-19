// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "encoder.h"
#include "identity.h"
#include "perf.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "cJSON.h"
#include "cbor.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "honch";

#define HONCH_MAX_BATCH_SIZE 50

// These are set by honch.c at init time.
extern const char *g_honch_device_model;
extern const char *g_honch_firmware_version;
extern const char *g_honch_environment;
extern int (*g_honch_battery_callback)(void);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} honch_buffer_t;

typedef struct {
    const char *event_name;
    const char *distinct_id;
    int64_t timestamp;
    const cJSON *properties;
} honch_event_encode_ctx_t;

static int64_t timestamp_millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

void honch_payload_free(honch_payload_t *payload)
{
    if (!payload) {
        return;
    }
    free(payload->data);
    payload->data = NULL;
    payload->len = 0;
}

static honch_err_t buffer_init(honch_buffer_t *buffer, size_t capacity)
{
    buffer->data = malloc(capacity);
    if (!buffer->data) {
        buffer->len = 0;
        buffer->capacity = 0;
        return HONCH_ERR_NO_MEM;
    }
    buffer->len = 0;
    buffer->capacity = capacity;
    return HONCH_OK;
}

static void buffer_free(honch_buffer_t *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->capacity = 0;
}

static honch_err_t buffer_reserve(honch_buffer_t *buffer, size_t extra)
{
    if (extra > SIZE_MAX - buffer->len) {
        return HONCH_ERR_NO_MEM;
    }
    size_t needed = buffer->len + extra;
    if (needed <= buffer->capacity) {
        return HONCH_OK;
    }

    size_t new_capacity = buffer->capacity ? buffer->capacity : 128;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    uint8_t *data = realloc(buffer->data, new_capacity);
    if (!data) {
        return HONCH_ERR_NO_MEM;
    }
    buffer->data = data;
    buffer->capacity = new_capacity;
    return HONCH_OK;
}

static honch_err_t buffer_append(honch_buffer_t *buffer, const void *data, size_t len)
{
    honch_err_t err = buffer_reserve(buffer, len);
    if (err != HONCH_OK) {
        return err;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return HONCH_OK;
}

static honch_err_t buffer_append_u8(honch_buffer_t *buffer, uint8_t value)
{
    return buffer_append(buffer, &value, 1);
}

static honch_err_t append_cbor_type(honch_buffer_t *buffer, uint8_t major, uint64_t value)
{
    if (value < 24) {
        return buffer_append_u8(buffer, major | (uint8_t)value);
    }

    if (value <= UINT8_MAX) {
        uint8_t bytes[2] = { major | 24, (uint8_t)value };
        return buffer_append(buffer, bytes, sizeof(bytes));
    }

    if (value <= UINT16_MAX) {
        uint8_t bytes[3] = {
            major | 25,
            (uint8_t)(value >> 8),
            (uint8_t)value,
        };
        return buffer_append(buffer, bytes, sizeof(bytes));
    }

    if (value <= UINT32_MAX) {
        uint8_t bytes[5] = {
            major | 26,
            (uint8_t)(value >> 24),
            (uint8_t)(value >> 16),
            (uint8_t)(value >> 8),
            (uint8_t)value,
        };
        return buffer_append(buffer, bytes, sizeof(bytes));
    }

    uint8_t bytes[9] = {
        major | 27,
        (uint8_t)(value >> 56),
        (uint8_t)(value >> 48),
        (uint8_t)(value >> 40),
        (uint8_t)(value >> 32),
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value,
    };
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static honch_err_t append_cbor_text(honch_buffer_t *buffer, const char *value)
{
    size_t len = strlen(value);
    honch_err_t err = append_cbor_type(buffer, 0x60, len);
    if (err != HONCH_OK) {
        return err;
    }
    return buffer_append(buffer, value, len);
}

static size_t cjson_child_count(const cJSON *value)
{
    size_t count = 0;
    for (const cJSON *child = value ? value->child : NULL; child; child = child->next) {
        count++;
    }
    return count;
}

static CborError encode_cjson_value(CborEncoder *encoder, const cJSON *value);

static CborError encode_cjson_array(CborEncoder *encoder, const cJSON *value)
{
    CborEncoder array_encoder;
    CborError err = cbor_encoder_create_array(encoder, &array_encoder, cjson_child_count(value));
    if (err != CborNoError) {
        return err;
    }

    for (const cJSON *child = value->child; child; child = child->next) {
        err = encode_cjson_value(&array_encoder, child);
        if (err != CborNoError) {
            return err;
        }
    }

    return cbor_encoder_close_container(encoder, &array_encoder);
}

static CborError encode_cjson_object(CborEncoder *encoder, const cJSON *value)
{
    size_t count = 0;
    for (const cJSON *child = value->child; child; child = child->next) {
        if (child->string) {
            count++;
        }
    }

    CborEncoder map_encoder;
    CborError err = cbor_encoder_create_map(encoder, &map_encoder, count);
    if (err != CborNoError) {
        return err;
    }

    for (const cJSON *child = value->child; child; child = child->next) {
        if (!child->string) {
            continue;
        }
        err = cbor_encode_text_stringz(&map_encoder, child->string);
        if (err != CborNoError) {
            return err;
        }
        err = encode_cjson_value(&map_encoder, child);
        if (err != CborNoError) {
            return err;
        }
    }

    return cbor_encoder_close_container(encoder, &map_encoder);
}

static CborError encode_cjson_number(CborEncoder *encoder, const cJSON *value)
{
    double number = value->valuedouble;
    if (number >= (double)INT64_MIN && number <= (double)INT64_MAX) {
        int64_t integer = (int64_t)number;
        if ((double)integer == number) {
            return cbor_encode_int(encoder, integer);
        }
    }
    return cbor_encode_double(encoder, number);
}

static CborError encode_cjson_value(CborEncoder *encoder, const cJSON *value)
{
    if (cJSON_IsNull(value)) {
        return cbor_encode_null(encoder);
    }
    if (cJSON_IsBool(value)) {
        return cbor_encode_boolean(encoder, cJSON_IsTrue(value));
    }
    if (cJSON_IsNumber(value)) {
        return encode_cjson_number(encoder, value);
    }
    if (cJSON_IsString(value)) {
        return cbor_encode_text_stringz(encoder, value->valuestring ? value->valuestring : "");
    }
    if (cJSON_IsArray(value)) {
        return encode_cjson_array(encoder, value);
    }
    if (cJSON_IsObject(value)) {
        return encode_cjson_object(encoder, value);
    }
    return cbor_encode_null(encoder);
}

static cJSON *build_properties(const char *properties_json)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    int64_t user_parse_us = 0;
    int64_t user_merge_us = 0;
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    cJSON *props = cJSON_CreateObject();
    if (!props) {
        return NULL;
    }

    if (properties_json && strlen(properties_json) > 0) {
        int64_t user_parse_start_us = HONCH_PERF_NOW_US();
        cJSON *user_props = cJSON_Parse(properties_json);
        user_parse_us = HONCH_PERF_ELAPSED_US(user_parse_start_us);
        if (user_props && cJSON_IsObject(user_props)) {
            int64_t user_merge_start_us = HONCH_PERF_NOW_US();
            for (cJSON *item = user_props->child; item; item = item->next) {
                if (!item->string) {
                    continue;
                }
                cJSON *copy = cJSON_Duplicate(item, true);
                if (!copy) {
                    cJSON_Delete(user_props);
                    cJSON_Delete(props);
                    return NULL;
                }
                cJSON_DeleteItemFromObject(props, item->string);
                cJSON_AddItemToObject(props, item->string, copy);
            }
            user_merge_us = HONCH_PERF_ELAPSED_US(user_merge_start_us);
        } else if (user_props) {
            ESP_LOGW(TAG, "properties_json must be an object, ignoring user properties");
        } else {
            ESP_LOGW(TAG, "Failed to parse properties_json, ignoring user properties");
        }
        cJSON_Delete(user_props);
    }

    int64_t sdk_enrich_start_us = HONCH_PERF_NOW_US();

    const char *device_id = honch_identity_get_device_id();
    cJSON_DeleteItemFromObject(props, "$device_id");
    cJSON_AddStringToObject(props, "$device_id", device_id ? device_id : "");

    cJSON_DeleteItemFromObject(props, "$device_model");
    cJSON_AddStringToObject(props, "$device_model", g_honch_device_model ? g_honch_device_model : "");

    cJSON_DeleteItemFromObject(props, "$firmware_version");
    cJSON_AddStringToObject(props, "$firmware_version",
                            g_honch_firmware_version ? g_honch_firmware_version : "");

    cJSON_DeleteItemFromObject(props, "$sdk_platform");
    cJSON_AddStringToObject(props, "$sdk_platform", "esp-idf");

    cJSON_DeleteItemFromObject(props, "$sdk_version");
    cJSON_AddStringToObject(props, "$sdk_version", "0.1.0");

    cJSON_DeleteItemFromObject(props, "$environment");
    cJSON_AddStringToObject(props, "$environment", g_honch_environment ? g_honch_environment : "production");

    const char *session_id = honch_identity_get_session_id();
    if (session_id) {
        cJSON_DeleteItemFromObject(props, "$session_id");
        cJSON_AddStringToObject(props, "$session_id", session_id);
    }

    if (g_honch_battery_callback) {
        int level = g_honch_battery_callback();
        if (level >= 0) {
            cJSON_DeleteItemFromObject(props, "$battery_level");
            cJSON_AddNumberToObject(props, "$battery_level", level);
        }
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_DeleteItemFromObject(props, "$wifi_rssi");
        cJSON_AddNumberToObject(props, "$wifi_rssi", ap_info.rssi);
    }

    HONCH_PERF_LOG("HONCH_PERF_PROPERTIES",
                   "total_us=%" PRId64 " user_parse_us=%" PRId64
                   " user_merge_us=%" PRId64 " sdk_enrich_us=%" PRId64
                   " prop_count=%u heap_before=%" PRIu32 " heap_after=%" PRIu32,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   user_parse_us,
                   user_merge_us,
                   HONCH_PERF_ELAPSED_US(sdk_enrich_start_us),
                   (unsigned)cjson_child_count(props),
                   heap_before,
                   HONCH_PERF_HEAP_FREE());

    return props;
}

static CborError encode_event_to_cbor(CborEncoder *encoder, void *userdata)
{
    honch_event_encode_ctx_t *ctx = (honch_event_encode_ctx_t *)userdata;

    CborEncoder map_encoder;
    CborError err = cbor_encoder_create_map(encoder, &map_encoder, 4);
    if (err != CborNoError) {
        return err;
    }

    err = cbor_encode_text_stringz(&map_encoder, "event");
    if (err == CborNoError) {
        err = cbor_encode_text_stringz(&map_encoder, ctx->event_name);
    }
    if (err == CborNoError) {
        err = cbor_encode_text_stringz(&map_encoder, "distinct_id");
    }
    if (err == CborNoError) {
        err = cbor_encode_text_stringz(&map_encoder, ctx->distinct_id);
    }
    if (err == CborNoError) {
        err = cbor_encode_text_stringz(&map_encoder, "timestamp");
    }
    if (err == CborNoError) {
        err = cbor_encode_int(&map_encoder, ctx->timestamp);
    }
    if (err == CborNoError) {
        err = cbor_encode_text_stringz(&map_encoder, "properties");
    }
    if (err == CborNoError) {
        err = encode_cjson_object(&map_encoder, ctx->properties);
    }
    if (err != CborNoError) {
        return err;
    }

    return cbor_encoder_close_container(encoder, &map_encoder);
}

static honch_err_t encode_with_retry(CborError (*encode_fn)(CborEncoder *, void *),
                                     void *userdata,
                                     honch_payload_t *out)
{
    size_t capacity = 512;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t *buffer = malloc(capacity);
        if (!buffer) {
            return HONCH_ERR_NO_MEM;
        }

        CborEncoder encoder;
        cbor_encoder_init(&encoder, buffer, capacity, 0);
        CborError err = encode_fn(&encoder, userdata);

        if (err == CborNoError) {
            out->data = buffer;
            out->len = cbor_encoder_get_buffer_size(&encoder, buffer);
            return HONCH_OK;
        }

        size_t extra = cbor_encoder_get_extra_bytes_needed(&encoder);
        free(buffer);

        if (err != CborErrorOutOfMemory) {
            ESP_LOGE(TAG, "CBOR encoding failed: %d", err);
            return HONCH_ERR_INTERNAL;
        }

        if (extra > 0 && extra <= SIZE_MAX - 64 && capacity <= SIZE_MAX - extra - 64) {
            capacity += extra + 64;
        } else if (capacity <= SIZE_MAX / 2) {
            capacity *= 2;
        } else {
            return HONCH_ERR_NO_MEM;
        }
    }

    return HONCH_ERR_NO_MEM;
}

honch_err_t honch_encode_event(const char *event_name,
                               const char *distinct_id,
                               const char *properties_json,
                               honch_payload_t *out)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!event_name || !distinct_id || !out) {
        return HONCH_ERR_INVALID_ARG;
    }

    out->data = NULL;
    out->len = 0;

    int64_t properties_start_us = HONCH_PERF_NOW_US();
    cJSON *properties = build_properties(properties_json);
    int64_t properties_us = HONCH_PERF_ELAPSED_US(properties_start_us);
    if (!properties) {
        return HONCH_ERR_NO_MEM;
    }

    honch_event_encode_ctx_t ctx = {
        .event_name = event_name,
        .distinct_id = distinct_id,
        .timestamp = timestamp_millis(),
        .properties = properties,
    };

    int64_t cbor_start_us = HONCH_PERF_NOW_US();
    honch_err_t err = encode_with_retry(encode_event_to_cbor, &ctx, out);
    int64_t cbor_us = HONCH_PERF_ELAPSED_US(cbor_start_us);
    cJSON_Delete(properties);

#ifdef CONFIG_HONCH_LOG_VERBOSE
    if (err == HONCH_OK) {
        ESP_LOGI(TAG, "Encoded CBOR event: %u bytes", (unsigned)out->len);
    }
#endif

    HONCH_PERF_LOG("HONCH_PERF_ENCODE_EVENT",
                   "event=%s result=%d total_us=%" PRId64
                   " properties_us=%" PRId64 " cbor_us=%" PRId64
                   " output_bytes=%u heap_before=%" PRIu32 " heap_after=%" PRIu32
                   " heap_min=%" PRIu32,
                   event_name,
                   err,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   properties_us,
                   cbor_us,
                   (unsigned)out->len,
                   heap_before,
                   HONCH_PERF_HEAP_FREE(),
                   HONCH_PERF_HEAP_MIN());

    return err;
}

honch_err_t honch_encode_batch(const char *api_key,
                               const honch_payload_t *events,
                               int count,
                               honch_payload_t *out)
{
    int64_t total_start_us = HONCH_PERF_NOW_US();
    uint32_t heap_before = HONCH_PERF_HEAP_FREE();

    if (!api_key || !events || !out || count <= 0 || count > HONCH_MAX_BATCH_SIZE) {
        return HONCH_ERR_INVALID_ARG;
    }

    out->data = NULL;
    out->len = 0;

    size_t capacity = strlen(api_key) + 32;
    size_t input_bytes = 0;
    for (int i = 0; i < count; i++) {
        if (!events[i].data || events[i].len == 0) {
            return HONCH_ERR_INVALID_ARG;
        }
        if (capacity > SIZE_MAX - events[i].len) {
            return HONCH_ERR_NO_MEM;
        }
        capacity += events[i].len;
        input_bytes += events[i].len;
    }

    honch_buffer_t buffer;
    int64_t alloc_start_us = HONCH_PERF_NOW_US();
    honch_err_t err = buffer_init(&buffer, capacity);
    int64_t alloc_us = HONCH_PERF_ELAPSED_US(alloc_start_us);
    if (err != HONCH_OK) {
        return err;
    }

    int64_t append_start_us = HONCH_PERF_NOW_US();
    err = append_cbor_type(&buffer, 0xA0, 2);
    if (err == HONCH_OK) {
        err = append_cbor_text(&buffer, "token");
    }
    if (err == HONCH_OK) {
        err = append_cbor_text(&buffer, api_key);
    }
    if (err == HONCH_OK) {
        err = append_cbor_text(&buffer, "batch");
    }
    if (err == HONCH_OK) {
        err = append_cbor_type(&buffer, 0x80, (uint64_t)count);
    }

    for (int i = 0; err == HONCH_OK && i < count; i++) {
        err = buffer_append(&buffer, events[i].data, events[i].len);
    }
    int64_t append_us = HONCH_PERF_ELAPSED_US(append_start_us);

    if (err != HONCH_OK) {
        buffer_free(&buffer);
        HONCH_PERF_LOG("HONCH_PERF_ENCODE_BATCH",
                       "result=%d count=%d input_bytes=%u total_us=%" PRId64
                       " alloc_us=%" PRId64 " append_us=%" PRId64
                       " heap_before=%" PRIu32 " heap_after=%" PRIu32,
                       err,
                       count,
                       (unsigned)input_bytes,
                       HONCH_PERF_ELAPSED_US(total_start_us),
                       alloc_us,
                       append_us,
                       heap_before,
                       HONCH_PERF_HEAP_FREE());
        return err;
    }

    out->data = buffer.data;
    out->len = buffer.len;
    HONCH_PERF_LOG("HONCH_PERF_ENCODE_BATCH",
                   "result=0 count=%d input_bytes=%u output_bytes=%u total_us=%" PRId64
                   " alloc_us=%" PRId64 " append_us=%" PRId64
                   " heap_before=%" PRIu32 " heap_after=%" PRIu32
                   " heap_min=%" PRIu32,
                   count,
                   (unsigned)input_bytes,
                   (unsigned)out->len,
                   HONCH_PERF_ELAPSED_US(total_start_us),
                   alloc_us,
                   append_us,
                   heap_before,
                   HONCH_PERF_HEAP_FREE(),
                   HONCH_PERF_HEAP_MIN());
    return HONCH_OK;
}
