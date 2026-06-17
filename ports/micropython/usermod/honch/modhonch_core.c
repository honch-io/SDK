#include "honch_micropython.h"
#include "honch_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct honch_micropython_client {
    mp_obj_base_t base;
    honch_client_t *client;
    honch_micropython_platform_t platform_ctx;
    honch_micropython_storage_t storage_ctx;
    honch_micropython_transport_t transport_ctx;
} honch_micropython_client_t;

extern const mp_obj_type_t honch_micropython_client_type;

#define HONCH_MP_MAX_TYPED_VALUES 64u
#define HONCH_MP_MAX_TYPED_PAIRS 64u
#define HONCH_MP_MAX_PROPERTIES 64u
#define DEFAULT_TRANSPORT_TIMEOUT_MS 2500u

typedef struct honch_mp_typed_pool {
    honch_value_t values[HONCH_MP_MAX_TYPED_VALUES];
    honch_map_pair_t pairs[HONCH_MP_MAX_TYPED_PAIRS];
    size_t value_count;
    size_t pair_count;
} honch_mp_typed_pool_t;

static honch_status_t honch_mp_value_from_obj(
    mp_obj_t obj,
    honch_mp_typed_pool_t *pool,
    honch_value_t *out,
    size_t depth);

static honch_status_t honch_mp_alloc_values(
    honch_mp_typed_pool_t *pool,
    size_t count,
    honch_value_t **out)
{
    if (pool == NULL || out == NULL || count > HONCH_MP_MAX_TYPED_VALUES - pool->value_count) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *out = &pool->values[pool->value_count];
    pool->value_count += count;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_alloc_pairs(
    honch_mp_typed_pool_t *pool,
    size_t count,
    honch_map_pair_t **out)
{
    if (pool == NULL || out == NULL || count > HONCH_MP_MAX_TYPED_PAIRS - pool->pair_count) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *out = &pool->pairs[pool->pair_count];
    pool->pair_count += count;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_value_from_map(
    mp_obj_t obj,
    honch_mp_typed_pool_t *pool,
    honch_value_t *out,
    size_t depth)
{
    mp_map_t *map = mp_obj_dict_get_map(obj);
    honch_map_pair_t *pairs = NULL;
    honch_status_t status = honch_mp_alloc_pairs(pool, map->used, &pairs);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    size_t pair_count = 0u;
    for (size_t i = 0u; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i)) {
            continue;
        }
        mp_map_elem_t *elem = &map->table[i];
        if (!mp_obj_is_str(elem->key)) {
            return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
        }
        pairs[pair_count].key = mp_obj_str_get_str(elem->key);
        status = honch_mp_value_from_obj(elem->value, pool, &pairs[pair_count].value, depth + 1u);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        pair_count++;
    }
    *out = honch_map(pairs, pair_count);
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_value_from_obj(
    mp_obj_t obj,
    honch_mp_typed_pool_t *pool,
    honch_value_t *out,
    size_t depth)
{
    if (pool == NULL || out == NULL || depth > 8u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (obj == mp_const_none) {
        *out = honch_null();
        return HONCH_STATUS_OK;
    }
    if (obj == mp_const_true || obj == mp_const_false) {
        *out = honch_bool(obj == mp_const_true);
        return HONCH_STATUS_OK;
    }
    if (mp_obj_is_int(obj)) {
        *out = honch_i64((int64_t)mp_obj_get_int(obj));
        return HONCH_STATUS_OK;
    }
#if MICROPY_PY_BUILTINS_FLOAT
    if (mp_obj_is_float(obj)) {
        *out = honch_f64((double)mp_obj_get_float(obj));
        return HONCH_STATUS_OK;
    }
#endif
    if (mp_obj_is_type(obj, &mp_type_bytes)) {
        size_t length = 0u;
        const char *value = mp_obj_str_get_data(obj, &length);
        *out = honch_bytes((const uint8_t *)value, length);
        return HONCH_STATUS_OK;
    }
    if (mp_obj_is_str(obj)) {
        size_t length = 0u;
        const char *value = mp_obj_str_get_data(obj, &length);
        *out = honch_strn(value, length);
        return HONCH_STATUS_OK;
    }
    if (mp_obj_is_type(obj, &mp_type_dict)) {
        return honch_mp_value_from_map(obj, pool, out, depth);
    }
    if (mp_obj_is_type(obj, &mp_type_tuple) || mp_obj_is_type(obj, &mp_type_list)) {
        size_t count = 0u;
        mp_obj_t *items_obj = NULL;
        mp_obj_get_array(obj, &count, &items_obj);
        honch_value_t *items = NULL;
        honch_status_t status = honch_mp_alloc_values(pool, count, &items);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        for (size_t i = 0u; i < count; i++) {
            status = honch_mp_value_from_obj(items_obj[i], pool, &items[i], depth + 1u);
            if (status != HONCH_STATUS_OK) {
                return status;
            }
        }
        *out = honch_array(items, count);
        return HONCH_STATUS_OK;
    }
    return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
}

static honch_status_t honch_mp_properties_from_obj(
    mp_obj_t obj,
    honch_mp_typed_pool_t *pool,
    honch_property_t *properties,
    size_t *property_count)
{
    if (obj == mp_const_none) {
        *property_count = 0u;
        return HONCH_STATUS_OK;
    }
    if (!mp_obj_is_type(obj, &mp_type_dict)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    mp_map_t *map = mp_obj_dict_get_map(obj);
    if (map->used > HONCH_MP_MAX_PROPERTIES) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    for (size_t i = 0u; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i)) {
            continue;
        }
        mp_map_elem_t *elem = &map->table[i];
        if (!mp_obj_is_str(elem->key)) {
            return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
        }
        properties[count].key = mp_obj_str_get_str(elem->key);
        honch_status_t status = honch_mp_value_from_obj(elem->value, pool, &properties[count].value, 0u);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        count++;
    }
    *property_count = count;
    return HONCH_STATUS_OK;
}

static const char *honch_mp_map_get_str(mp_obj_t dict_obj, qstr key, const char *fallback)
{
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return fallback;
    }
    return mp_obj_str_get_str(elem->value);
}

static honch_status_t honch_mp_map_get_error_severity(mp_obj_t dict_obj, honch_error_severity_t *out)
{
    if (out == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    const char *severity = honch_mp_map_get_str(dict_obj, MP_QSTR_severity, "error");
    if (strcmp(severity, "info") == 0) {
        *out = HONCH_ERROR_SEVERITY_INFO;
        return HONCH_STATUS_OK;
    }
    if (strcmp(severity, "warning") == 0) {
        *out = HONCH_ERROR_SEVERITY_WARNING;
        return HONCH_STATUS_OK;
    }
    if (strcmp(severity, "error") == 0) {
        *out = HONCH_ERROR_SEVERITY_ERROR;
        return HONCH_STATUS_OK;
    }
    if (strcmp(severity, "fatal") == 0) {
        *out = HONCH_ERROR_SEVERITY_FATAL;
        return HONCH_STATUS_OK;
    }
    return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
}

static size_t honch_mp_map_get_size(mp_obj_t dict_obj, qstr key, size_t fallback)
{
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return fallback;
    }
    mp_int_t value = mp_obj_get_int(elem->value);
    if (value <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("size config values must be positive"));
    }
    return (size_t)value;
}

static unsigned int honch_mp_map_get_uint(mp_obj_t dict_obj, qstr key, unsigned int fallback)
{
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return fallback;
    }
    int value = mp_obj_get_int(elem->value);
    return value < 0 ? fallback : (unsigned int)value;
}

static honch_status_t honch_mp_map_get_writable_buffer(mp_obj_t dict_obj, qstr key, uint8_t **buffer, size_t *buffer_size)
{
    if (buffer == NULL || buffer_size == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *buffer = NULL;
    *buffer_size = 0u;
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    mp_buffer_info_t info;
    mp_get_buffer_raise(elem->value, &info, MP_BUFFER_WRITE);
    if (info.buf == NULL || info.len == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *buffer = (uint8_t *)info.buf;
    *buffer_size = info.len;
    return HONCH_STATUS_OK;
}

static mp_obj_t honch_client_make_new(
    const mp_obj_type_t *type,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    if (!mp_obj_is_type(args[0], &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("config must be a dict"));
    }

    honch_micropython_client_t *self = mp_obj_malloc(honch_micropython_client_t, type);
    memset(&self->platform_ctx, 0, sizeof(self->platform_ctx));
    memset(&self->storage_ctx, 0, sizeof(self->storage_ctx));
    memset(&self->transport_ctx, 0, sizeof(self->transport_ctx));
    self->client = NULL;

    honch_platform_ops_t platform_ops;
    honch_event_queue_ops_t event_queue_ops;
    honch_transport_ops_t transport_ops;
    uint8_t *event_buffer = NULL;
    size_t event_buffer_size = 0u;

    HONCH_MP_DEBUG_INIT("platform_ops_begin");
    honch_status_t status = honch_micropython_platform_ops_init(&platform_ops, &self->platform_ctx);
    HONCH_MP_DEBUG_INIT("platform_ops_done");
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_map_get_writable_buffer(args[0], MP_QSTR_event_buffer, &event_buffer, &event_buffer_size);
    }
    if (status == HONCH_STATUS_OK) {
        /* Size the queue's entry metadata to the same cap the core enforces
         * (max_queued_events, defaulting to HONCH_DEFAULT_MAX_QUEUED_EVENTS)
         * rather than buffer_size/16, which can be many times larger. */
        size_t storage_max_events = honch_mp_map_get_size(args[0], MP_QSTR_max_queued_events, 0);
        if (storage_max_events == 0u) {
            storage_max_events = HONCH_DEFAULT_MAX_QUEUED_EVENTS;
        }
        HONCH_MP_DEBUG_INIT("storage_ops_begin");
        status = honch_micropython_storage_ops_init(
            &event_queue_ops, &self->storage_ctx, event_buffer, event_buffer_size, storage_max_events);
        HONCH_MP_DEBUG_INIT("storage_ops_done");
    }
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("transport_ops_begin");
        status = honch_micropython_transport_ops_init(
            &transport_ops,
            &self->transport_ctx,
            honch_mp_map_get_uint(args[0], MP_QSTR_transport_timeout_ms, DEFAULT_TRANSPORT_TIMEOUT_MS));
        HONCH_MP_DEBUG_INIT("transport_ops_done");
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_storage_ops_deinit(&self->storage_ctx);
        honch_micropython_raise_status(status);
    }

    honch_core_config_t config = {
        .api_key = honch_mp_map_get_str(args[0], MP_QSTR_api_key, NULL),
        .endpoint_url = honch_mp_map_get_str(args[0], MP_QSTR_endpoint_url, NULL),
        .device_id = honch_mp_map_get_str(args[0], MP_QSTR_device_id, NULL),
        .device_model = honch_mp_map_get_str(args[0], MP_QSTR_device_model, NULL),
        .firmware_version = honch_mp_map_get_str(args[0], MP_QSTR_firmware_version, NULL),
        .environment = honch_mp_map_get_str(args[0], MP_QSTR_environment, NULL),
        .sdk_platform = "micropython",
        .queue_directory = "",
        .batch_size = honch_mp_map_get_size(args[0], MP_QSTR_batch_size, 0),
        .max_queued_events = honch_mp_map_get_size(args[0], MP_QSTR_max_queued_events, 0),
        .max_event_bytes = honch_mp_map_get_size(args[0], MP_QSTR_max_event_bytes, 0),
        .transport_timeout_ms = honch_mp_map_get_uint(args[0], MP_QSTR_transport_timeout_ms, DEFAULT_TRANSPORT_TIMEOUT_MS),
        .flush_interval_seconds = honch_mp_map_get_uint(args[0], MP_QSTR_flush_interval_seconds, 0),
        .flush_min_interval_ms = honch_mp_map_get_uint(args[0], MP_QSTR_flush_min_interval_ms, 0),
        .flush_event_threshold = honch_mp_map_get_size(args[0], MP_QSTR_flush_event_threshold, 0),
        .flush_retry_initial_ms = honch_mp_map_get_uint(args[0], MP_QSTR_flush_retry_initial_ms, 0),
        .flush_retry_max_ms = honch_mp_map_get_uint(args[0], MP_QSTR_flush_retry_max_ms, 0),
        .battery_callback = NULL,
        .battery_low_threshold = honch_mp_map_get_uint(args[0], MP_QSTR_battery_low_threshold, 0),
        .platform = &platform_ops,
        .state_storage = NULL,
        .event_queue = &event_queue_ops,
        .transport = &transport_ops,
    };

    HONCH_MP_DEBUG_INIT("core_init_begin");
    status = honch_core_init(&self->client, &config);
    HONCH_MP_DEBUG_INIT("core_init_done");
    if (status != HONCH_STATUS_OK) {
        honch_micropython_storage_ops_deinit(&self->storage_ctx);
        honch_micropython_raise_status(status);
    }

    return MP_OBJ_FROM_PTR(self);
}

static honch_micropython_client_t *honch_get_self(mp_obj_t self_in)
{
    return MP_OBJ_TO_PTR(self_in);
}

static mp_obj_t honch_client_track(mp_obj_t self_in, mp_obj_t event_in, mp_obj_t properties_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_mp_typed_pool_t pool = {0};
    honch_property_t properties[HONCH_MP_MAX_PROPERTIES];
    size_t property_count = 0u;
    honch_status_t status = honch_mp_properties_from_obj(properties_in, &pool, properties, &property_count);
    if (status == HONCH_STATUS_OK) {
        status = honch_core_track(
            self->client,
            mp_obj_str_get_str(event_in),
            properties,
            property_count);
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_track_obj, honch_client_track);

static mp_obj_t honch_client_report_error(mp_obj_t self_in, mp_obj_t report_in, mp_obj_t properties_in)
{
    if (!mp_obj_is_type(report_in, &mp_type_dict)) {
        honch_micropython_raise_status(HONCH_STATUS_ERROR_INVALID_ARGUMENT);
        return mp_const_none;
    }
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_mp_typed_pool_t pool = {0};
    honch_property_t properties[HONCH_MP_MAX_PROPERTIES];
    size_t property_count = 0u;
    honch_error_report_t report = {0};
    honch_status_t status = honch_mp_map_get_error_severity(report_in, &report.severity);
    if (status == HONCH_STATUS_OK) {
        report.message = honch_mp_map_get_str(report_in, MP_QSTR_message, NULL);
        report.type = honch_mp_map_get_str(report_in, MP_QSTR_type, NULL);
        report.component = honch_mp_map_get_str(report_in, MP_QSTR_component, NULL);
        report.code = honch_mp_map_get_str(report_in, MP_QSTR_code, NULL);
        report.backtrace = honch_mp_map_get_str(report_in, MP_QSTR_backtrace, NULL);
        status = honch_mp_properties_from_obj(properties_in, &pool, properties, &property_count);
    }
    if (status == HONCH_STATUS_OK) {
        status = honch_core_report_error(self->client, &report, properties, property_count);
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_report_error_obj, honch_client_report_error);

static mp_obj_t honch_client_identify(mp_obj_t self_in, mp_obj_t distinct_id_in, mp_obj_t traits_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_mp_typed_pool_t pool = {0};
    honch_property_t traits[HONCH_MP_MAX_PROPERTIES];
    size_t trait_count = 0u;
    honch_status_t status = honch_mp_properties_from_obj(traits_in, &pool, traits, &trait_count);
    if (status == HONCH_STATUS_OK) {
        status = honch_core_identify(self->client,
            mp_obj_str_get_str(distinct_id_in),
            traits,
            trait_count);
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_identify_obj, honch_client_identify);

static mp_obj_t honch_client_set_property(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_mp_typed_pool_t pool = {0};
    honch_value_t value;
    honch_status_t status = honch_mp_value_from_obj(value_in, &pool, &value, 0u);
    if (status == HONCH_STATUS_OK) {
        status = honch_core_set_property(
            self->client,
            mp_obj_str_get_str(key_in),
            value);
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_set_property_obj, honch_client_set_property);

static mp_obj_t honch_client_session_start(mp_obj_t self_in, mp_obj_t session_name_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    const char *session_name = session_name_in == mp_const_none ? NULL : mp_obj_str_get_str(session_name_in);
    honch_status_t status = honch_core_session_start(self->client, session_name);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(honch_client_session_start_obj, honch_client_session_start);

static mp_obj_t honch_client_session_end(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_session_end(self->client);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_session_end_obj, honch_client_session_end);

static mp_obj_t honch_client_connectivity_changed(mp_obj_t self_in, mp_obj_t connected_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    const honch_property_t properties[] = {
        honch_prop("state", honch_str(mp_obj_is_true(connected_in) ? "connected" : "disconnected"))
    };
    honch_status_t status = honch_core_track(self->client, "$connectivity_change", properties, 1u);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(honch_client_connectivity_changed_obj, honch_client_connectivity_changed);

static mp_obj_t honch_client_tick(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_tick(self->client);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_tick_obj, honch_client_tick);

static mp_obj_t honch_client_flush(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_flush(self->client);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_flush_obj, honch_client_flush);

static mp_obj_t honch_client_reset(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_reset(self->client);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_reset_obj, honch_client_reset);

static mp_obj_t honch_client_shutdown(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    if (self->client != NULL) {
        honch_status_t status = honch_core_shutdown(self->client);
        if (status == HONCH_STATUS_ERROR_BUSY) {
            honch_micropython_raise_status(status);
        }
        self->client = NULL;
        honch_micropython_storage_ops_deinit(&self->storage_ctx);
        if (status != HONCH_STATUS_OK) {
            honch_micropython_raise_status(status);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_shutdown_obj, honch_client_shutdown);

static mp_obj_t honch_client_get_device_id(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    const char *device_id = honch_core_get_device_id(self->client);
    if (device_id == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_str(device_id, strlen(device_id));
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_get_device_id_obj, honch_client_get_device_id);

static mp_obj_t honch_client_queue_stats(mp_obj_t self_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_queue_stats_t stats = {0};
    honch_status_t status = honch_core_get_queue_stats(self->client, &stats);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    mp_obj_t dict = mp_obj_new_dict(7);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_depth), mp_obj_new_int_from_ull(stats.queued_events));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_bytes_used), mp_obj_new_int_from_ull(stats.queued_bytes));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_bytes_capacity), mp_obj_new_int_from_ull(stats.buffer_bytes));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_capacity_dropped_events), mp_obj_new_int_from_ull(stats.capacity_dropped_events));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_oversized_rejected_events), mp_obj_new_int_from_ull(stats.oversized_rejected_events));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dead_lettered_events), mp_obj_new_int_from_ull(stats.dead_lettered_events));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_high_watermark_depth), mp_obj_new_int_from_ull(stats.high_water_events));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(honch_client_queue_stats_obj, honch_client_queue_stats);

static const mp_rom_map_elem_t honch_client_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_track), MP_ROM_PTR(&honch_client_track_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_error), MP_ROM_PTR(&honch_client_report_error_obj) },
    { MP_ROM_QSTR(MP_QSTR_identify), MP_ROM_PTR(&honch_client_identify_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_property), MP_ROM_PTR(&honch_client_set_property_obj) },
    { MP_ROM_QSTR(MP_QSTR_session_start), MP_ROM_PTR(&honch_client_session_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_session_end), MP_ROM_PTR(&honch_client_session_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_connectivity_changed), MP_ROM_PTR(&honch_client_connectivity_changed_obj) },
    { MP_ROM_QSTR(MP_QSTR_tick), MP_ROM_PTR(&honch_client_tick_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&honch_client_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&honch_client_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&honch_client_shutdown_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_device_id), MP_ROM_PTR(&honch_client_get_device_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue_stats), MP_ROM_PTR(&honch_client_queue_stats_obj) },
};
static MP_DEFINE_CONST_DICT(honch_client_locals_dict, honch_client_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    honch_micropython_client_type,
    MP_QSTR_Client,
    MP_TYPE_FLAG_NONE,
    make_new, honch_client_make_new,
    locals_dict, &honch_client_locals_dict
    );

static const mp_rom_map_elem_t honch_core_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__honch_core) },
    { MP_ROM_QSTR(MP_QSTR_Client), MP_ROM_PTR(&honch_micropython_client_type) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_INVALID_ARGUMENT), MP_ROM_INT(HONCH_STATUS_ERROR_INVALID_ARGUMENT) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_IO), MP_ROM_INT(HONCH_STATUS_ERROR_IO) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_TRANSPORT), MP_ROM_INT(HONCH_STATUS_ERROR_TRANSPORT) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_RATE_LIMITED), MP_ROM_INT(HONCH_STATUS_ERROR_RATE_LIMITED) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_SERVER), MP_ROM_INT(HONCH_STATUS_ERROR_SERVER) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_REJECTED), MP_ROM_INT(HONCH_STATUS_ERROR_REJECTED) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_NOT_INITIALIZED), MP_ROM_INT(HONCH_STATUS_ERROR_NOT_INITIALIZED) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_QUEUE_FULL), MP_ROM_INT(HONCH_STATUS_ERROR_QUEUE_FULL) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_TIMEOUT), MP_ROM_INT(HONCH_STATUS_ERROR_TIMEOUT) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_BUSY), MP_ROM_INT(HONCH_STATUS_ERROR_BUSY) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_NOT_SUPPORTED), MP_ROM_INT(HONCH_STATUS_ERROR_NOT_SUPPORTED) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_OFFLINE), MP_ROM_INT(HONCH_STATUS_ERROR_OFFLINE) },
};
static MP_DEFINE_CONST_DICT(honch_core_module_globals, honch_core_module_globals_table);

const mp_obj_module_t honch_core_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&honch_core_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__honch_core, honch_core_user_cmodule);
