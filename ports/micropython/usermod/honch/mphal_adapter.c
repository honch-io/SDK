#include "honch_micropython.h"
#include "honch_internal.h"

#include <stdlib.h>
#include <string.h>

char *honch_micropython_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, length + 1u);
    return copy;
}

honch_status_t honch_micropython_join_path(char **out, const char *left, const char *right)
{
    if (out == NULL || left == NULL || right == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    int needs_separator = left_length > 0u && left[left_length - 1u] != '/';
    size_t total = left_length + (needs_separator ? 1u : 0u) + right_length + 1u;
    char *joined = (char *)malloc(total);
    if (joined == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    memcpy(joined, left, left_length);
    size_t offset = left_length;
    if (needs_separator) {
        joined[offset++] = '/';
    }
    memcpy(joined + offset, right, right_length);
    joined[offset + right_length] = '\0';
    *out = joined;
    return HONCH_STATUS_OK;
}

honch_status_t honch_micropython_call_noarg_attr(mp_obj_t object, qstr attr)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    mp_obj_t method = mp_load_attr(object, attr);
    mp_call_function_0(method);
    nlr_pop();
    return HONCH_STATUS_OK;
}

void honch_micropython_raise_status(honch_status_t status)
{
    if (status == HONCH_STATUS_OK) {
        return;
    }
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), honch_status_string(status));
}

static uint64_t honch_mp_now_ms(void *ctx)
{
    (void)ctx;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        nlr_buf_t fallback;
        if (nlr_push(&fallback) != 0) {
            return 0u;
        }
        mp_obj_t utime_module = mp_import_name(MP_QSTR_utime, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t ticks_ms = mp_load_attr(utime_module, MP_QSTR_ticks_ms);
        mp_obj_t value = mp_call_function_0(ticks_ms);
        nlr_pop();
        return (uint64_t)mp_obj_get_int(value);
    }
    mp_obj_t time_module = mp_import_name(MP_QSTR_time, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t ticks_ms = mp_load_attr(time_module, MP_QSTR_ticks_ms);
    mp_obj_t value = mp_call_function_0(ticks_ms);
    nlr_pop();
    return (uint64_t)mp_obj_get_int(value);
}

static uint64_t honch_mp_uptime_ms(void *ctx)
{
    return honch_mp_now_ms(ctx);
}

static honch_status_t honch_mp_random_bytes(void *ctx, uint8_t *buffer, size_t buffer_size)
{
    (void)ctx;
    if (buffer == NULL && buffer_size > 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        nlr_buf_t fallback;
        if (nlr_push(&fallback) != 0) {
            return HONCH_STATUS_ERROR_IO;
        }
        mp_obj_t random_module = mp_import_name(MP_QSTR_random, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t getrandbits = mp_load_attr(random_module, MP_QSTR_getrandbits);
        for (size_t i = 0u; i < buffer_size; i++) {
            buffer[i] = (uint8_t)mp_obj_get_int(mp_call_function_1(getrandbits, MP_OBJ_NEW_SMALL_INT(8)));
        }
        nlr_pop();
        return HONCH_STATUS_OK;
    }
    mp_obj_t random_module = mp_import_name(MP_QSTR_urandom, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t getrandbits = mp_load_attr(random_module, MP_QSTR_getrandbits);
    for (size_t i = 0u; i < buffer_size; i++) {
        buffer[i] = (uint8_t)mp_obj_get_int(mp_call_function_1(getrandbits, MP_OBJ_NEW_SMALL_INT(8)));
    }
    nlr_pop();
    return HONCH_STATUS_OK;
}

static void honch_mp_log(void *ctx, honch_log_level_t level, const char *message)
{
    (void)ctx;
    (void)level;
    (void)message;
}

honch_status_t honch_micropython_platform_ops_init(
    honch_platform_ops_t *ops,
    honch_micropython_platform_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *ctx = (honch_micropython_platform_t) {0};
    *ops = (honch_platform_ops_t) {
        .now_ms = honch_mp_now_ms,
        .uptime_ms = honch_mp_uptime_ms,
        .random_bytes = honch_mp_random_bytes,
        .log = honch_mp_log,
        .ctx = ctx,
    };
    return HONCH_STATUS_OK;
}

uint64_t honch_now_millis(void)
{
    return honch_mp_now_ms(NULL);
}

honch_status_t honch_random_hex(char out[33])
{
    if (out == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    uint8_t bytes[16];
    honch_status_t status = honch_mp_random_bytes(NULL, bytes, sizeof(bytes));
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(bytes); i++) {
        out[i * 2u] = hex[(bytes[i] >> 4u) & 0x0fu];
        out[(i * 2u) + 1u] = hex[bytes[i] & 0x0fu];
    }
    out[32] = '\0';
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_prepare(honch_client_t *client, const honch_core_config_t *config)
{
    HONCH_MP_DEBUG_INIT("state_prepare_begin");
    if (client == NULL || config == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    if (config->device_id == NULL || honch_is_blank(config->device_id)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    client->configured_device_id = true;
    client->device_id = honch_micropython_strdup(config->device_id);
    if (client->device_id == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }

    client->distinct_id = honch_micropython_strdup(client->device_id);
    client->device_model = honch_micropython_strdup(config->device_model);
    client->firmware_version = honch_micropython_strdup(config->firmware_version);
    client->environment = honch_micropython_strdup(honch_is_blank(config->environment) ? "production" : config->environment);
    if (client->distinct_id == NULL || client->device_model == NULL || client->firmware_version == NULL || client->environment == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    HONCH_MP_DEBUG_INIT("state_prepare_done");
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_save_distinct_id(honch_client_t *client)
{
    return honch_state_save_distinct_id_value(client, client != NULL ? client->distinct_id : NULL);
}

honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinct_id)
{
    if (client == NULL || distinct_id == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_check_firmware_version(honch_client_t *client, bool *changed, char **previous_version)
{
    if (client == NULL || changed == NULL || previous_version == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *changed = false;
    *previous_version = NULL;
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_save_firmware_version(honch_client_t *client)
{
    if (client == NULL || honch_is_blank(client->firmware_version)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_reset(honch_client_t *client)
{
    if (client == NULL || client->device_id == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *distinct_id = honch_micropython_strdup(client->device_id);
    if (distinct_id == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    free(client->distinct_id);
    client->distinct_id = distinct_id;
    return HONCH_STATUS_OK;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size)
{
    if (client == NULL || client->event_queue == NULL || client->event_queue->queue_push == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (client->sequence == UINT64_MAX) {
        return HONCH_STATUS_ERROR_QUEUE_FULL;
    }
    return client->event_queue->queue_push(client->event_queue->ctx, event, event_size, client->sequence++);
}

honch_status_t honch_queue_clear(honch_client_t *client)
{
    if (client == NULL || client->event_queue == NULL || client->event_queue->queue_clear == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return client->event_queue->queue_clear(client->event_queue->ctx);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count)
{
    if (client == NULL || client->event_queue == NULL || client->event_queue->queue_depth == NULL || count == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return client->event_queue->queue_depth(client->event_queue->ctx, count);
}
