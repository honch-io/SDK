#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static honch_status_t honch_list_queue_files(honch_client_t *client, honch_file_list_t *files)
{
    return honch_list_files_with_suffix(client->pending_directory, ".json", files);
}

static honch_status_t honch_move_to_dead(honch_client_t *client, const honch_file_entry_t *entry)
{
    char *dead_path = NULL;
    honch_status_t status = honch_join_path(&dead_path, client->dead_directory, entry->name);
    if (status != HONCH_OK) {
        return status;
    }

    if (rename(entry->path, dead_path) != 0) {
        free(dead_path);
        return HONCH_ERROR_IO;
    }

    free(dead_path);
    return HONCH_OK;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const char *event_json)
{
    if (strlen(event_json) > client->max_event_bytes) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    if (status != HONCH_OK) {
        honch_file_list_free(&files);
        return status;
    }

    while (files.count >= client->max_queued_events && files.count > 0u) {
        status = honch_unlink_if_exists(files.items[0].path);
        if (status != HONCH_OK) {
            honch_file_list_free(&files);
            return status;
        }

        honch_file_list_free(&files);
        files = (honch_file_list_t){0};
        status = honch_list_queue_files(client, &files);
        if (status != HONCH_OK) {
            honch_file_list_free(&files);
            return status;
        }
    }
    honch_file_list_free(&files);

    char event_id[33];
    status = honch_random_hex(event_id);
    if (status != HONCH_OK) {
        return status;
    }

    char filename[96];
    snprintf(
        filename,
        sizeof(filename),
        "%020llu-%06llu-%s.json",
        (unsigned long long)honch_now_millis(),
        (unsigned long long)client->sequence++,
        event_id);

    return honch_write_file_atomic(client->pending_directory, filename, event_json);
}

static honch_status_t honch_build_batch(const honch_file_list_t *files, size_t count, char **out)
{
    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&buffer, "{\"batch\":[");
    for (size_t i = 0u; status == HONCH_OK && i < count; i++) {
        char *event_json = NULL;
        status = honch_read_file(files->items[i].path, &event_json);
        if (status != HONCH_OK) {
            break;
        }

        if (i > 0u) {
            status = honch_buffer_append(&buffer, ",");
        }
        if (status == HONCH_OK) {
            status = honch_buffer_append(&buffer, event_json);
        }
        free(event_json);
    }

    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, "]}");
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    *out = buffer.data;
    return HONCH_OK;
}

static honch_status_t honch_delete_files(const honch_file_list_t *files, size_t count)
{
    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; i < count; i++) {
        status = honch_unlink_if_exists(files->items[i].path);
        if (status != HONCH_OK) {
            break;
        }
    }
    return status;
}

static honch_status_t honch_dead_letter_files(honch_client_t *client, const honch_file_list_t *files, size_t count)
{
    honch_status_t status = HONCH_OK;
    for (size_t i = 0u; i < count; i++) {
        status = honch_move_to_dead(client, &files->items[i]);
        if (status != HONCH_OK) {
            break;
        }
    }
    return status;
}

honch_status_t honch_queue_flush_locked(honch_client_t *client)
{
    honch_status_t final_status = HONCH_OK;

    for (;;) {
        honch_file_list_t files = {0};
        honch_status_t status = honch_list_queue_files(client, &files);
        if (status != HONCH_OK || files.count == 0u) {
            honch_file_list_free(&files);
            return status == HONCH_OK ? final_status : status;
        }

        size_t count = files.count < client->batch_size ? files.count : client->batch_size;
        char *payload = NULL;
        status = honch_build_batch(&files, count, &payload);
        if (status != HONCH_OK) {
            honch_file_list_free(&files);
            return status;
        }

        honch_http_result_t result = HONCH_HTTP_RETRY;
        status = honch_transport_post_batch(client, payload, &result);
        free(payload);

        if (result == HONCH_HTTP_OK) {
            status = honch_delete_files(&files, count);
            honch_file_list_free(&files);
            if (status != HONCH_OK) {
                return status;
            }
            continue;
        }

        if (result == HONCH_HTTP_REJECTED) {
            honch_status_t dead_status = honch_dead_letter_files(client, &files, count);
            honch_file_list_free(&files);
            return dead_status == HONCH_OK ? status : dead_status;
        }

        honch_file_list_free(&files);
        return status;
    }
}
