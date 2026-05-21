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

static honch_status_t honch_mp_lock(void *ctx)
{
    (void)ctx;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_unlock(void *ctx)
{
    (void)ctx;
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
        .lock = honch_mp_lock,
        .unlock = honch_mp_unlock,
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
    if (client == NULL || config == NULL || client->storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    if (config->device_id != NULL && !honch_is_blank(config->device_id)) {
        HONCH_MP_DEBUG_INIT("state_device_id_config_begin");
        client->configured_device_id = true;
        client->device_id = honch_micropython_strdup(config->device_id);
        if (client->device_id == NULL) {
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        honch_status_t write_status = client->storage->state_set(
            client->storage->ctx,
            "device_id",
            (const uint8_t *)client->device_id,
            strlen(client->device_id));
        if (write_status != HONCH_STATUS_OK) {
            return write_status;
        }
        HONCH_MP_DEBUG_INIT("state_device_id_config_done");
    } else {
        HONCH_MP_DEBUG_INIT("state_device_id_load_begin");
        size_t value_size = 0u;
        honch_status_t status = client->storage->state_get(client->storage->ctx, "device_id", NULL, &value_size);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        if (value_size > 0u) {
            client->device_id = (char *)malloc(value_size + 1u);
            if (client->device_id == NULL) {
                return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
            }
            status = client->storage->state_get(client->storage->ctx, "device_id", (uint8_t *)client->device_id, &value_size);
            if (status != HONCH_STATUS_OK) {
                return status;
            }
            client->device_id[value_size] = '\0';
        } else {
            char generated[33];
            status = honch_random_hex(generated);
            if (status != HONCH_STATUS_OK) {
                return status;
            }
            client->device_id = honch_micropython_strdup(generated);
            if (client->device_id == NULL) {
                return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
            }
            status = client->storage->state_set(client->storage->ctx, "device_id", (const uint8_t *)client->device_id, strlen(client->device_id));
            if (status != HONCH_STATUS_OK) {
                return status;
            }
        }
        HONCH_MP_DEBUG_INIT("state_device_id_load_done");
    }

    HONCH_MP_DEBUG_INIT("state_distinct_begin");
    size_t distinct_size = 0u;
    honch_status_t status = client->storage->state_get(client->storage->ctx, "distinct_id", NULL, &distinct_size);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    if (distinct_size > 0u) {
        client->distinct_id = (char *)malloc(distinct_size + 1u);
        if (client->distinct_id == NULL) {
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        status = client->storage->state_get(client->storage->ctx, "distinct_id", (uint8_t *)client->distinct_id, &distinct_size);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        client->distinct_id[distinct_size] = '\0';
    } else {
        client->distinct_id = honch_micropython_strdup(client->device_id);
        if (client->distinct_id == NULL) {
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        status = honch_state_save_distinct_id(client);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
    }
    HONCH_MP_DEBUG_INIT("state_distinct_done");

    client->device_model = honch_micropython_strdup(config->device_model);
    client->firmware_version = honch_micropython_strdup(config->firmware_version);
    client->environment = honch_micropython_strdup(honch_is_blank(config->environment) ? "production" : config->environment);
    if (client->device_model == NULL || client->firmware_version == NULL || client->environment == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    HONCH_MP_DEBUG_INIT("state_prepare_done");
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_save_distinct_id(honch_client_t *client)
{
    return honch_state_save_distinct_id_value(client, client->distinct_id);
}

honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinct_id)
{
    if (client == NULL || client->storage == NULL || distinct_id == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return client->storage->state_set(client->storage->ctx, "distinct_id", (const uint8_t *)distinct_id, strlen(distinct_id));
}

honch_status_t honch_state_check_firmware_version(honch_client_t *client, bool *changed, char **previous_version)
{
    HONCH_MP_DEBUG_INIT("firmware_check_begin");
    if (client == NULL || changed == NULL || previous_version == NULL || client->storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *changed = false;
    *previous_version = NULL;

    size_t value_size = 0u;
    honch_status_t status = client->storage->state_get(client->storage->ctx, "firmware_version", NULL, &value_size);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    if (value_size > 0u) {
        char *stored = (char *)malloc(value_size + 1u);
        if (stored == NULL) {
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        status = client->storage->state_get(client->storage->ctx, "firmware_version", (uint8_t *)stored, &value_size);
        if (status != HONCH_STATUS_OK) {
            free(stored);
            return status;
        }
        stored[value_size] = '\0';
        if (strcmp(stored, client->firmware_version) != 0) {
            *changed = true;
            *previous_version = stored;
            stored = NULL;
        }
        free(stored);
    }
    honch_status_t write_status = client->storage->state_set(client->storage->ctx, "firmware_version", (const uint8_t *)client->firmware_version, strlen(client->firmware_version));
    HONCH_MP_DEBUG_INIT("firmware_check_done");
    return write_status;
}

honch_status_t honch_state_reset(honch_client_t *client)
{
    if (client == NULL || client->storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = HONCH_STATUS_OK;
    char *device_id = NULL;
    char *distinct_id = NULL;
    if (client->configured_device_id) {
        device_id = honch_micropython_strdup(client->device_id);
        distinct_id = honch_micropython_strdup(client->device_id);
        if (device_id == NULL || distinct_id == NULL) {
            free(device_id);
            free(distinct_id);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
    } else {
        char generated[33];
        status = honch_random_hex(generated);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        device_id = honch_micropython_strdup(generated);
        distinct_id = honch_micropython_strdup(generated);
        if (device_id == NULL || distinct_id == NULL) {
            free(device_id);
            free(distinct_id);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
    }
    status = client->storage->state_set(client->storage->ctx, "device_id", (const uint8_t *)device_id, strlen(device_id));
    if (status == HONCH_STATUS_OK) {
        status = client->storage->state_set(client->storage->ctx, "distinct_id", (const uint8_t *)distinct_id, strlen(distinct_id));
    }
    if (status != HONCH_STATUS_OK) {
        free(device_id);
        free(distinct_id);
        return status;
    }
    free(client->device_id);
    free(client->distinct_id);
    client->device_id = device_id;
    client->distinct_id = distinct_id;
    return HONCH_STATUS_OK;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_push == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return client->storage->queue_push(client->storage->ctx, event, event_size, client->sequence++);
}

honch_status_t honch_queue_clear(honch_client_t *client)
{
    return client->storage->queue_clear(client->storage->ctx);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count)
{
    return client->storage->queue_depth(client->storage->ctx, count);
}
