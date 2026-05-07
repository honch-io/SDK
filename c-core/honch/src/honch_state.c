#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char *path = NULL;
    honch_status_t status = honch_state_path(client, name, &path);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_read_file(path, out);
    free(path);
    if (status == HONCH_ERROR_IO) {
        *out = NULL;
        return HONCH_OK;
    }
    if (status == HONCH_OK && *out != NULL) {
        honch_trim_trailing_ws(*out);
    }
    return status;
}

static honch_status_t honch_write_state_file(honch_client_t *client, const char *name, const char *content)
{
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

static honch_status_t honch_hex_filename(const char *key, char **out)
{
    static const char hex[] = "0123456789abcdef";
    size_t key_length = strlen(key);
    size_t filename_length = (key_length * 2u) + 6u;
    char *filename = (char *)malloc(filename_length);
    if (filename == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    for (size_t i = 0u; i < key_length; i++) {
        unsigned char ch = (unsigned char)key[i];
        filename[i * 2u] = hex[(ch >> 4u) & 0x0fu];
        filename[(i * 2u) + 1u] = hex[ch & 0x0fu];
    }
    memcpy(filename + (key_length * 2u), ".prop", 6u);
    *out = filename;
    return HONCH_OK;
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

honch_status_t honch_state_prepare(honch_client_t *client, const honch_config_t *config)
{
    honch_status_t status = honch_join_path(&client->pending_directory, client->queue_directory, "pending");
    if (status == HONCH_OK) {
        status = honch_join_path(&client->dead_directory, client->queue_directory, "dead");
    }
    if (status == HONCH_OK) {
        status = honch_join_path(&client->state_directory, client->queue_directory, "state");
    }
    if (status == HONCH_OK) {
        status = honch_join_path(&client->properties_directory, client->state_directory, "properties");
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
        status = honch_mkdir_p(client->properties_directory);
    }
    if (status == HONCH_OK) {
        status = honch_remove_tmp_files(client->pending_directory);
    }
    if (status == HONCH_OK) {
        status = honch_remove_tmp_files(client->state_directory);
    }
    if (status == HONCH_OK) {
        status = honch_remove_tmp_files(client->properties_directory);
    }
    if (status != HONCH_OK) {
        return status;
    }

    char *stored_device_id = NULL;
    if (config->device_id != NULL && !honch_is_blank(config->device_id)) {
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
    return honch_write_state_file(client, "distinct_id", client->distinct_id);
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

honch_status_t honch_state_set_property(honch_client_t *client, const char *key, const char *value_json)
{
    char *filename = NULL;
    honch_status_t status = honch_hex_filename(key, &filename);
    if (status != HONCH_OK) {
        return status;
    }

    honch_buffer_t record;
    status = honch_buffer_init(&record, strlen(key) + strlen(value_json) + 32u);
    if (status == HONCH_OK) {
        status = honch_buffer_append(&record, "{\"key\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&record, key);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&record, ",\"value\":");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&record, value_json);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&record, "}");
    }
    if (status == HONCH_OK) {
        status = honch_write_file_atomic(client->properties_directory, filename, record.data);
    }

    honch_buffer_free(&record);
    free(filename);
    return status;
}

static honch_status_t honch_append_property_record(
    honch_buffer_t *buffer,
    const char *record_json,
    bool *has_members)
{
    const char *key_marker = "\"key\":";
    const char *value_marker = "\"value\":";
    const char *key_start = strstr(record_json, key_marker);
    const char *value_start = strstr(record_json, value_marker);
    if (key_start == NULL || value_start == NULL || value_start <= key_start) {
        return HONCH_ERROR_IO;
    }

    key_start += strlen(key_marker);
    while (*key_start == ' ' || *key_start == '\t' || *key_start == '\n' || *key_start == '\r') {
        key_start++;
    }
    const char *key_end = value_start;
    while (key_end > key_start &&
           (key_end[-1] == ' ' || key_end[-1] == '\t' || key_end[-1] == '\n' || key_end[-1] == '\r' ||
            key_end[-1] == ',')) {
        key_end--;
    }

    if (key_end > key_start + 1 && key_start[0] == '"' && key_end[-1] == '"') {
        size_t key_length = (size_t)(key_end - key_start - 2);
        if (key_length <= HONCH_MAX_PROPERTY_KEY) {
            char key[HONCH_MAX_PROPERTY_KEY + 1u];
            memcpy(key, key_start + 1, key_length);
            key[key_length] = '\0';
            if (honch_property_key_is_reserved(key)) {
                return HONCH_OK;
            }
        }
    }

    value_start += strlen(value_marker);
    while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n' || *value_start == '\r') {
        value_start++;
    }
    const char *value_end = record_json + strlen(record_json);
    while (value_end > value_start &&
           (value_end[-1] == ' ' || value_end[-1] == '\t' ||
            value_end[-1] == '\n' || value_end[-1] == '\r')) {
        value_end--;
    }
    if (value_end > value_start && value_end[-1] == '}') {
        value_end--;
    }
    while (value_end > value_start &&
           (value_end[-1] == ' ' || value_end[-1] == '\t' ||
            value_end[-1] == '\n' || value_end[-1] == '\r')) {
        value_end--;
    }

    honch_status_t status = HONCH_OK;
    if (*has_members) {
        status = honch_buffer_append(buffer, ",");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(buffer, key_start, (size_t)(key_end - key_start));
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(buffer, ":");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(buffer, value_start, (size_t)(value_end - value_start));
    }
    if (status == HONCH_OK) {
        *has_members = true;
    }
    return status;
}

honch_status_t honch_state_append_properties(honch_client_t *client, honch_buffer_t *buffer, bool *has_members)
{
    honch_file_list_t files = {0};
    honch_status_t status = honch_list_files_with_suffix(client->properties_directory, ".prop", &files);
    if (status != HONCH_OK) {
        honch_file_list_free(&files);
        return status;
    }

    for (size_t i = 0u; status == HONCH_OK && i < files.count; i++) {
        char *record = NULL;
        status = honch_read_file(files.items[i].path, &record);
        if (status == HONCH_OK) {
            status = honch_append_property_record(buffer, record, has_members);
        }
        free(record);
    }

    honch_file_list_free(&files);
    return status;
}

honch_status_t honch_state_reset(honch_client_t *client)
{
    char next_device_id[33];
    honch_status_t status = honch_random_hex(next_device_id);
    if (status != HONCH_OK) {
        return status;
    }

    char *device_id = honch_strdup(next_device_id);
    if (device_id == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    char *distinct_id = honch_strdup(next_device_id);
    if (distinct_id == NULL) {
        free(device_id);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    status = honch_write_state_file(client, "device_id", device_id);
    if (status == HONCH_OK) {
        status = honch_write_state_file(client, "distinct_id", distinct_id);
    }

    if (status == HONCH_OK) {
        honch_file_list_t properties = {0};
        status = honch_list_files_with_suffix(client->properties_directory, ".prop", &properties);
        for (size_t i = 0u; status == HONCH_OK && i < properties.count; i++) {
            status = honch_unlink_if_exists(properties.items[i].path);
        }
        honch_file_list_free(&properties);
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
