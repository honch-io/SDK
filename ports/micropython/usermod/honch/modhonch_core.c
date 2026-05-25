#include "honch_micropython.h"

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

static const char *honch_mp_map_get_str(mp_obj_t dict_obj, qstr key, const char *fallback)
{
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return fallback;
    }
    return mp_obj_str_get_str(elem->value);
}

static size_t honch_mp_map_get_size(mp_obj_t dict_obj, qstr key, size_t fallback)
{
    mp_map_t *map = mp_obj_dict_get_map(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL || elem->value == mp_const_none) {
        return fallback;
    }
    return (size_t)mp_obj_get_int(elem->value);
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

    const char *queue_directory = honch_mp_map_get_str(args[0], MP_QSTR_queue_directory, NULL);
    honch_platform_ops_t platform_ops;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport_ops;

    HONCH_MP_DEBUG_INIT("platform_ops_begin");
    honch_status_t status = honch_micropython_platform_ops_init(&platform_ops, &self->platform_ctx);
    HONCH_MP_DEBUG_INIT("platform_ops_done");
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("storage_ops_begin");
        status = honch_micropython_storage_ops_init(&storage_ops, &self->storage_ctx, queue_directory);
        HONCH_MP_DEBUG_INIT("storage_ops_done");
    }
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("transport_ops_begin");
        status = honch_micropython_transport_ops_init(
            &transport_ops,
            &self->transport_ctx,
            honch_mp_map_get_uint(args[0], MP_QSTR_transport_timeout_ms, 0));
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
        .queue_directory = queue_directory,
        .batch_size = honch_mp_map_get_size(args[0], MP_QSTR_batch_size, 0),
        .max_queued_events = honch_mp_map_get_size(args[0], MP_QSTR_max_queued_events, 0),
        .max_event_bytes = honch_mp_map_get_size(args[0], MP_QSTR_max_event_bytes, 0),
        .transport_timeout_ms = honch_mp_map_get_uint(args[0], MP_QSTR_transport_timeout_ms, 0),
        .flush_interval_seconds = honch_mp_map_get_uint(args[0], MP_QSTR_flush_interval_seconds, 0),
        .flush_event_threshold = honch_mp_map_get_size(args[0], MP_QSTR_flush_event_threshold, 0),
        .flush_retry_initial_ms = honch_mp_map_get_uint(args[0], MP_QSTR_flush_retry_initial_ms, 0),
        .flush_retry_max_ms = honch_mp_map_get_uint(args[0], MP_QSTR_flush_retry_max_ms, 0),
        .disable_background_flush = 1,
        .battery_callback = NULL,
        .battery_low_threshold = honch_mp_map_get_uint(args[0], MP_QSTR_battery_low_threshold, 0),
        .durability_mode = HONCH_DURABILITY_OS_BUFFERED,
        .platform = &platform_ops,
        .storage = &storage_ops,
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
    honch_status_t status = honch_core_track(
        self->client,
        mp_obj_str_get_str(event_in),
        mp_obj_str_get_str(properties_in));
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_track_obj, honch_client_track);

static mp_obj_t honch_client_identify(mp_obj_t self_in, mp_obj_t distinct_id_in, mp_obj_t traits_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_identify(self->client,
        mp_obj_str_get_str(distinct_id_in),
        mp_obj_str_get_str(traits_in));
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(honch_client_identify_obj, honch_client_identify);

static mp_obj_t honch_client_set_property(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value_in)
{
    honch_micropython_client_t *self = honch_get_self(self_in);
    honch_status_t status = honch_core_set_property(
        self->client,
        mp_obj_str_get_str(key_in),
        mp_obj_str_get_str(value_in));
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
    const char *properties = mp_obj_is_true(connected_in) ?
        "{\"state\":\"connected\"}" :
        "{\"state\":\"disconnected\"}";
    honch_status_t status = honch_core_track(self->client, "$connectivity_change", properties);
    if (status != HONCH_STATUS_OK) {
        honch_micropython_raise_status(status);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(honch_client_connectivity_changed_obj, honch_client_connectivity_changed);

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

static const mp_rom_map_elem_t honch_client_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_track), MP_ROM_PTR(&honch_client_track_obj) },
    { MP_ROM_QSTR(MP_QSTR_identify), MP_ROM_PTR(&honch_client_identify_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_property), MP_ROM_PTR(&honch_client_set_property_obj) },
    { MP_ROM_QSTR(MP_QSTR_session_start), MP_ROM_PTR(&honch_client_session_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_session_end), MP_ROM_PTR(&honch_client_session_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_connectivity_changed), MP_ROM_PTR(&honch_client_connectivity_changed_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&honch_client_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&honch_client_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&honch_client_shutdown_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_device_id), MP_ROM_PTR(&honch_client_get_device_id_obj) },
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
};
static MP_DEFINE_CONST_DICT(honch_core_module_globals, honch_core_module_globals_table);

const mp_obj_module_t honch_core_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&honch_core_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__honch_core, honch_core_user_cmodule);
