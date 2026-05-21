#include "honch_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static honch_status_t honch_state_path(honch_client_t *client, const char *name, char **out)
{
    return honch_join_path(out, client->state_directory, name);
}

static void honch_trim_trailing_ws(char *value)
{
    size_t length = strlen(value);
    while (length > 0u &&
           (value[length - 1u] == ' ' || value[length - 1u] == '\t' ||
            value[length - 1u] == '\n' || value[length - 1u] == '\r')) {
        value[length - 1u] = '\0';
        length--;
    }
}

static honch_status_t honch_read_optional_state_file(
    honch_client_t *client,
    const char *name,
    char **out)
{
    *out = NULL;
    if (client->storage != NULL && client->storage->state_get != NULL) {
        size_t value_size = 0u;
        honch_status_t status = client->storage->state_get(client->storage->ctx, name, NULL, &value_size);
        if (status != HONCH_OK || value_size == 0u) {
            return status;
        }

        char *value = (char *)malloc(value_size + 1u);
        if (value == NULL) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }

        size_t read_size = value_size;
        status = client->storage->state_get(client->storage->ctx, name, (uint8_t *)value, &read_size);
        if (status != HONCH_OK) {
            free(value);
            return status;
        }
        value[read_size] = '\0';
        honch_trim_trailing_ws(value);
        *out = value;
        return HONCH_OK;
    }

    char *path = NULL;
    honch_status_t status = honch_state_path(client, name, &path);
    if (status != HONCH_OK) {
        return status;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        int saved_errno = errno;
        free(path);
        if (saved_errno == ENOENT) {
            *out = NULL;
            return HONCH_OK;
        }
        return HONCH_ERROR_IO;
    }
    if (!S_ISREG(info.st_mode)) {
        free(path);
        return HONCH_ERROR_IO;
    }

    status = honch_read_file(path, out);
    free(path);
    if (status == HONCH_OK && *out != NULL) {
        honch_trim_trailing_ws(*out);
    }
    return status;
}

static honch_status_t honch_write_state_file(honch_client_t *client, const char *name, const char *content)
{
    if (client->storage != NULL && client->storage->state_set != NULL) {
        return client->storage->state_set(
            client->storage->ctx,
            name,
            (const uint8_t *)content,
            strlen(content));
    }

    return honch_write_file_atomic(client->state_directory, name, content);
}

static honch_status_t honch_copy_or_null(char **out, const char *value)
{
    *out = NULL;
    if (value == NULL) {
        return HONCH_OK;
    }

    *out = honch_strdup(value);
    return *out == NULL ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
}

static honch_status_t honch_remove_tmp_files(const char *directory)
{
    honch_file_list_t tmp_files = {0};
    honch_status_t status = honch_list_files_with_suffix(directory, ".tmp", &tmp_files);
    if (status != HONCH_OK) {
        honch_file_list_free(&tmp_files);
        return status;
    }

    for (size_t i = 0u; i < tmp_files.count; i++) {
        honch_status_t unlink_status = honch_unlink_if_exists(tmp_files.items[i].path);
        if (unlink_status != HONCH_OK) {
            status = unlink_status;
            break;
        }
    }

    honch_file_list_free(&tmp_files);
    return status;
}

honch_status_t honch_state_prepare(honch_client_t *client, const honch_core_config_t *config)
{
    honch_status_t status = honch_join_path(&client->pending_directory, client->queue_directory, "pending");
    if (status == HONCH_OK) {
        status = honch_join_path(&client->dead_directory, client->queue_directory, "dead");
    }
    if (status == HONCH_OK) {
        status = honch_join_path(&client->state_directory, client->queue_directory, "state");
    }
    if (status == HONCH_OK) {
        status = honch_mkdir_p(client->pending_directory);
    }
    if (status == HONCH_OK) {
        status = honch_mkdir_p(client->dead_directory);
    }
    if (status == HONCH_OK) {
        status = honch_mkdir_p(client->state_directory);
    }
    if (status == HONCH_OK) {
        status = honch_remove_tmp_files(client->pending_directory);
    }
    if (status == HONCH_OK) {
        status = honch_remove_tmp_files(client->state_directory);
    }
    if (status != HONCH_OK) {
        return status;
    }

    char *stored_device_id = NULL;
    if (config->device_id != NULL && !honch_is_blank(config->device_id)) {
        client->configured_device_id = true;
        client->device_id = honch_strdup(config->device_id);
        if (client->device_id == NULL) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        status = honch_write_state_file(client, "device_id", client->device_id);
    } else {
        status = honch_read_optional_state_file(client, "device_id", &stored_device_id);
        if (status == HONCH_OK && !honch_is_blank(stored_device_id)) {
            client->device_id = stored_device_id;
            stored_device_id = NULL;
        } else if (status == HONCH_OK) {
            char generated[33];
            status = honch_random_hex(generated);
            if (status == HONCH_OK) {
                client->device_id = honch_strdup(generated);
                if (client->device_id == NULL) {
                    status = HONCH_ERROR_OUT_OF_MEMORY;
                }
            }
            if (status == HONCH_OK) {
                status = honch_write_state_file(client, "device_id", client->device_id);
            }
        }
    }
    free(stored_device_id);
    if (status != HONCH_OK) {
        return status;
    }

    char *stored_distinct_id = NULL;
    status = honch_read_optional_state_file(client, "distinct_id", &stored_distinct_id);
    if (status != HONCH_OK) {
        return status;
    }
    if (!honch_is_blank(stored_distinct_id)) {
        client->distinct_id = stored_distinct_id;
        stored_distinct_id = NULL;
    } else {
        client->distinct_id = honch_strdup(client->device_id);
        if (client->distinct_id == NULL) {
            status = HONCH_ERROR_OUT_OF_MEMORY;
        } else {
            status = honch_state_save_distinct_id(client);
        }
    }
    free(stored_distinct_id);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_copy_or_null(&client->device_model, config->device_model);
    if (status == HONCH_OK) {
        status = honch_copy_or_null(&client->firmware_version, config->firmware_version);
    }
    if (status == HONCH_OK && !honch_is_blank(config->environment)) {
        status = honch_copy_or_null(&client->environment, config->environment);
    } else if (status == HONCH_OK) {
        client->environment = honch_strdup("production");
        if (client->environment == NULL) {
            status = HONCH_ERROR_OUT_OF_MEMORY;
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
    return honch_write_state_file(client, "distinct_id", distinct_id);
}

honch_status_t honch_state_check_firmware_version(honch_client_t *client, bool *changed, char **previous_version)
{
    *changed = false;
    *previous_version = NULL;
    if (honch_is_blank(client->firmware_version)) {
        return HONCH_OK;
    }

    char *stored_version = NULL;
    honch_status_t status = honch_read_optional_state_file(client, "firmware_version", &stored_version);
    if (status != HONCH_OK) {
        return status;
    }

    if (!honch_is_blank(stored_version) && strcmp(stored_version, client->firmware_version) != 0) {
        *previous_version = stored_version;
        stored_version = NULL;
        *changed = true;
    }

    status = honch_write_state_file(client, "firmware_version", client->firmware_version);
    if (status != HONCH_OK) {
        free(*previous_version);
        *previous_version = NULL;
        *changed = false;
    }

    free(stored_version);
    return status;
}

honch_status_t honch_state_reset(honch_client_t *client)
{
    honch_status_t status = HONCH_OK;
    char *device_id = NULL;
    char *distinct_id = NULL;
    if (client->configured_device_id) {
        device_id = honch_strdup(client->device_id);
        distinct_id = honch_strdup(client->device_id);
        if (device_id == NULL || distinct_id == NULL) {
            free(device_id);
            free(distinct_id);
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
    } else {
        char next_device_id[33];
        status = honch_random_hex(next_device_id);
        if (status != HONCH_OK) {
            return status;
        }
        device_id = honch_strdup(next_device_id);
        if (device_id == NULL) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        distinct_id = honch_strdup(next_device_id);
        if (distinct_id == NULL) {
            free(device_id);
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
    }

    status = honch_write_state_file(client, "device_id", device_id);
    if (status == HONCH_OK) {
        status = honch_write_state_file(client, "distinct_id", distinct_id);
    }

    if (status == HONCH_OK) {
        free(client->device_id);
        free(client->distinct_id);
        client->device_id = device_id;
        client->distinct_id = distinct_id;
        device_id = NULL;
        distinct_id = NULL;
    }

    free(device_id);
    free(distinct_id);
    return status;
}
