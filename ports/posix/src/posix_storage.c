#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static honch_status_t honch_list_queue_files(honch_client_t *client, honch_file_list_t *files)
{
    return honch_list_files_with_suffix(client->pending_directory, ".cbor", files);
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

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size)
{
    if (event == NULL || event_size == 0u || event_size > client->max_event_bytes) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (client->queued_event_count >= client->max_queued_events) {
        honch_file_list_t files = {0};
        honch_status_t status = honch_list_queue_files(client, &files);
        if (status != HONCH_OK) {
            honch_file_list_free(&files);
            return status;
        }

        client->queued_event_count = files.count;
        for (size_t i = 0u; client->queued_event_count >= client->max_queued_events && i < files.count; i++) {
            status = honch_unlink_if_exists(files.items[i].path);
            if (status != HONCH_OK) {
                honch_file_list_free(&files);
                return status;
            }
            client->queued_event_count--;
        }
        honch_file_list_free(&files);
    }

    char event_id[33];
    honch_status_t status = honch_random_hex(event_id);
    if (status != HONCH_OK) {
        return status;
    }

    char filename[96];
    snprintf(
        filename,
        sizeof(filename),
        "%020llu-%06llu-%s.cbor",
        (unsigned long long)honch_now_millis(),
        (unsigned long long)client->sequence++,
        event_id);

    status = honch_write_file_atomic_bytes_with_durability(
        client->pending_directory,
        filename,
        event,
        event_size,
        client->durability_mode);
    if (status == HONCH_OK) {
        client->queued_event_count++;
    }
    return status;
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count)
{
    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    if (status == HONCH_OK) {
        *count = files.count;
    }
    honch_file_list_free(&files);
    return status;
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

static honch_status_t honch_delete_queue_directory(const char *directory)
{
    honch_file_list_t files = {0};
    honch_status_t status = honch_list_files_with_suffix(directory, ".cbor", &files);
    if (status == HONCH_OK) {
        status = honch_delete_files(&files, files.count);
    }
    honch_file_list_free(&files);
    return status;
}

honch_status_t honch_queue_clear(honch_client_t *client)
{
    honch_status_t status = honch_delete_queue_directory(client->pending_directory);
    if (status == HONCH_OK) {
        status = honch_delete_queue_directory(client->dead_directory);
    }
    if (status == HONCH_OK) {
        client->queued_event_count = 0u;
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

static void honch_file_list_remove_at(honch_file_list_t *files, size_t index)
{
    if (files == NULL || index >= files->count) {
        return;
    }

    free(files->items[index].name);
    free(files->items[index].path);
    for (size_t i = index + 1u; i < files->count; i++) {
        files->items[i - 1u] = files->items[i];
    }
    files->count--;
    if (files->count > 0u) {
        files->items[files->count].name = NULL;
        files->items[files->count].path = NULL;
    }
}

honch_status_t honch_queue_flush_locked(honch_client_t *client)
{
    honch_status_t final_status = HONCH_OK;
    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    if (status == HONCH_OK) {
        client->queued_event_count = files.count;
    }
    if (status != HONCH_OK || files.count == 0u) {
        honch_file_list_free(&files);
        return status;
    }

    size_t index = 0u;
    while (index < files.count) {
        size_t remaining = files.count - index;
        size_t count = remaining < client->batch_size ? remaining : client->batch_size;
        honch_payload_t payload = {0};
        size_t invalid_index = count;
        honch_file_list_t batch = {
            .items = files.items + index,
            .count = count,
            .capacity = count
        };
        status = honch_encoder_build_batch_cbor(client, &batch, count, &payload, &invalid_index);
        if (status == HONCH_ERROR_INVALID_ARGUMENT && invalid_index < count) {
            size_t absolute_invalid_index = index + invalid_index;
            honch_status_t dead_status = honch_move_to_dead(client, &files.items[absolute_invalid_index]);
            if (dead_status == HONCH_OK) {
                honch_file_list_remove_at(&files, absolute_invalid_index);
                client->queued_event_count = files.count > index ? files.count - index : 0u;
            }
            if (dead_status != HONCH_OK) {
                honch_file_list_free(&files);
                return dead_status;
            }
            final_status = HONCH_ERROR_REJECTED;
            continue;
        }
        if (status != HONCH_OK) {
            honch_file_list_free(&files);
            return status;
        }

        honch_http_result_t result = HONCH_HTTP_RETRY;
        status = honch_transport_post_batch(client, payload.data, payload.length, &result);
        free(payload.data);

        if (result == HONCH_HTTP_OK) {
            status = honch_delete_files(&batch, count);
            if (status == HONCH_OK) {
                index += count;
                client->queued_event_count = files.count > index ? files.count - index : 0u;
            }
            if (status != HONCH_OK) {
                honch_file_list_free(&files);
                return status;
            }
            continue;
        }

        if (result == HONCH_HTTP_REJECTED) {
            honch_status_t dead_status = honch_dead_letter_files(client, &batch, count);
            if (dead_status == HONCH_OK) {
                client->queued_event_count = files.count > index + count ? files.count - index - count : 0u;
            }
            honch_file_list_free(&files);
            return dead_status == HONCH_OK ? status : dead_status;
        }

        honch_file_list_free(&files);
        return status;
    }

    honch_file_list_free(&files);
    return final_status;
}
