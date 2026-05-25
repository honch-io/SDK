// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"

#include "honch_internal.h"

static honch_status_t honch_esp_state_get_string(
    honch_client_t *client,
    const char *key,
    char **out)
{
    *out = NULL;
    if (client == NULL || client->storage == NULL || client->storage->state_get == NULL || key == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    size_t value_size = 0u;
    honch_status_t status = client->storage->state_get(client->storage->ctx, key, NULL, &value_size);
    if (status != HONCH_STATUS_OK || value_size == 0u) {
        return status;
    }

    size_t alloc_size = 0u;
    if (honch_size_add(value_size, 1u, &alloc_size) != HONCH_STATUS_OK) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    char *value = (char *)malloc(alloc_size);
    if (value == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }

    size_t read_size = value_size;
    status = client->storage->state_get(client->storage->ctx, key, (uint8_t *)value, &read_size);
    if (status != HONCH_STATUS_OK) {
        free(value);
        return status;
    }
    if (read_size > value_size) {
        free(value);
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    value[read_size] = '\0';
    *out = value;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_esp_write_state_string(
    honch_client_t *client,
    const char *key,
    const char *value)
{
    if (client == NULL || client->storage == NULL || client->storage->state_set == NULL ||
        key == NULL || value == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->state_set(
        client->storage->ctx,
        key,
        (const uint8_t *)value,
        strlen(value));
}

static honch_status_t honch_esp_copy_or_null(char **out, const char *value)
{
    *out = NULL;
    if (value == NULL) {
        return HONCH_STATUS_OK;
    }

    *out = honch_strdup(value);
    return *out == NULL ? HONCH_STATUS_ERROR_OUT_OF_MEMORY : HONCH_STATUS_OK;
}

uint64_t honch_now_millis(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

honch_status_t honch_random_hex(char out[33])
{
    if (out == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));

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
    if (client == NULL || config == NULL || client->storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = HONCH_STATUS_OK;
    char *stored_device_id = NULL;
    if (config->device_id != NULL && !honch_is_blank(config->device_id)) {
        client->configured_device_id = true;
        client->device_id = honch_strdup(config->device_id);
        if (client->device_id == NULL) {
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        status = honch_esp_write_state_string(client, "device_id", client->device_id);
    } else {
        status = honch_esp_state_get_string(client, "device_id", &stored_device_id);
        if (status == HONCH_STATUS_OK && !honch_is_blank(stored_device_id)) {
            client->device_id = stored_device_id;
            stored_device_id = NULL;
        } else if (status == HONCH_STATUS_OK) {
            char generated[33];
            status = honch_random_hex(generated);
            if (status == HONCH_STATUS_OK) {
                client->device_id = honch_strdup(generated);
                if (client->device_id == NULL) {
                    status = HONCH_STATUS_ERROR_OUT_OF_MEMORY;
                }
            }
            if (status == HONCH_STATUS_OK) {
                status = honch_esp_write_state_string(client, "device_id", client->device_id);
            }
        }
    }
    free(stored_device_id);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    char *stored_distinct_id = NULL;
    status = honch_esp_state_get_string(client, "distinct_id", &stored_distinct_id);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    if (!honch_is_blank(stored_distinct_id)) {
        client->distinct_id = stored_distinct_id;
        stored_distinct_id = NULL;
    } else {
        client->distinct_id = honch_strdup(client->device_id);
        if (client->distinct_id == NULL) {
            status = HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        } else {
            status = honch_state_save_distinct_id(client);
        }
    }
    free(stored_distinct_id);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    status = honch_esp_copy_or_null(&client->device_model, config->device_model);
    if (status == HONCH_STATUS_OK) {
        status = honch_esp_copy_or_null(&client->firmware_version, config->firmware_version);
    }
    if (status == HONCH_STATUS_OK && !honch_is_blank(config->environment)) {
        status = honch_esp_copy_or_null(&client->environment, config->environment);
    } else if (status == HONCH_STATUS_OK) {
        client->environment = honch_strdup("production");
        if (client->environment == NULL) {
            status = HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
    }

    return status;
}

honch_status_t honch_state_save_distinct_id(honch_client_t *client)
{
    return honch_state_save_distinct_id_value(client, client->distinct_id);
}

honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinct_id)
{
    return honch_esp_write_state_string(client, "distinct_id", distinct_id);
}

honch_status_t honch_state_check_firmware_version(
    honch_client_t *client,
    bool *changed,
    char **previous_version)
{
    if (client == NULL || changed == NULL || previous_version == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *changed = false;
    *previous_version = NULL;
    if (honch_is_blank(client->firmware_version)) {
        return HONCH_STATUS_OK;
    }

    char *stored_version = NULL;
    honch_status_t status = honch_esp_state_get_string(client, "firmware_version", &stored_version);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    if (!honch_is_blank(stored_version) && strcmp(stored_version, client->firmware_version) != 0) {
        *previous_version = stored_version;
        stored_version = NULL;
        *changed = true;
    }

    free(stored_version);
    return HONCH_STATUS_OK;
}

honch_status_t honch_state_save_firmware_version(honch_client_t *client)
{
    if (client == NULL || honch_is_blank(client->firmware_version)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return honch_esp_write_state_string(client, "firmware_version", client->firmware_version);
}

honch_status_t honch_state_reset(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = HONCH_STATUS_OK;
    char *previous_device_id = honch_strdup(client->device_id);
    char *previous_distinct_id = honch_strdup(client->distinct_id);
    char *device_id = NULL;
    char *distinct_id = NULL;
    if (previous_device_id == NULL || previous_distinct_id == NULL) {
        free(previous_device_id);
        free(previous_distinct_id);
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }

    if (client->configured_device_id) {
        device_id = honch_strdup(client->device_id);
        distinct_id = honch_strdup(client->device_id);
        if (device_id == NULL || distinct_id == NULL) {
            free(device_id);
            free(distinct_id);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
    } else {
        char next_device_id[33];
        status = honch_random_hex(next_device_id);
        if (status != HONCH_STATUS_OK) {
            return status;
        }
        device_id = honch_strdup(next_device_id);
        distinct_id = honch_strdup(next_device_id);
        if (device_id == NULL || distinct_id == NULL) {
            free(device_id);
            free(distinct_id);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
    }

    status = honch_esp_write_state_string(client, "device_id", device_id);
    if (status == HONCH_STATUS_OK) {
        status = honch_esp_write_state_string(client, "distinct_id", distinct_id);
        if (status != HONCH_STATUS_OK) {
            honch_status_t rollback_status = honch_esp_write_state_string(client, "device_id", previous_device_id);
            if (rollback_status != HONCH_STATUS_OK) {
                status = rollback_status;
            }
        }
    }

    if (status == HONCH_STATUS_OK) {
        free(client->device_id);
        free(client->distinct_id);
        client->device_id = device_id;
        client->distinct_id = distinct_id;
        device_id = NULL;
        distinct_id = NULL;
    }

    free(previous_device_id);
    free(previous_distinct_id);
    free(device_id);
    free(distinct_id);
    return status;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_push == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (client->sequence == UINT64_MAX) {
        return HONCH_STATUS_ERROR_QUEUE_FULL;
    }

    return client->storage->queue_push(client->storage->ctx, event, event_size, client->sequence++);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_depth == NULL || count == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_depth(client->storage->ctx, count);
}

honch_status_t honch_queue_clear(honch_client_t *client)
{
    if (client == NULL || client->storage == NULL || client->storage->queue_clear == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    return client->storage->queue_clear(client->storage->ctx);
}
