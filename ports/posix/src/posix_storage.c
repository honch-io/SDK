#include "honch_internal.h"
#include "honch/posix/honch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static honch_status_t honch_posix_storage_missing_client(void)
{
    return HONCH_ERROR_NOT_INITIALIZED;
}

static honch_status_t honch_posix_state_path(honch_client_t *client, const char *key, char **out)
{
    return honch_join_path(out, client->state_directory, key);
}

static honch_status_t honch_posix_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || honch_is_blank(key) || buffer_size == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    char *path = NULL;
    honch_status_t status = honch_posix_state_path(client, key, &path);
    if (status != HONCH_OK) {
        return status;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        int saved_errno = errno;
        free(path);
        if (saved_errno == ENOENT) {
            *buffer_size = 0u;
            return HONCH_OK;
        }
        return HONCH_ERROR_IO;
    }
    if (!S_ISREG(info.st_mode)) {
        free(path);
        return HONCH_ERROR_IO;
    }

    char *content = NULL;
    status = honch_read_file(path, &content);
    free(path);
    if (status != HONCH_OK) {
        return status;
    }

    size_t content_size = strlen(content);
    if (buffer == NULL || *buffer_size < content_size) {
        *buffer_size = content_size;
        free(content);
        return buffer == NULL ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
    }

    memcpy(buffer, content, content_size);
    *buffer_size = content_size;
    free(content);
    return HONCH_OK;
}

static honch_status_t honch_posix_state_set(void *ctx, const char *key, const uint8_t *data, size_t data_size)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || honch_is_blank(key) || (data == NULL && data_size > 0u)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    return honch_write_file_atomic_bytes(client->state_directory, key, data, data_size);
}

static honch_status_t honch_posix_state_delete(void *ctx, const char *key)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || honch_is_blank(key)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    char *path = NULL;
    honch_status_t status = honch_posix_state_path(client, key, &path);
    if (status == HONCH_OK) {
        status = honch_unlink_if_exists(path);
    }
    free(path);
    return status;
}

static honch_status_t honch_queue_enqueue_with_sequence(
    honch_client_t *client,
    const unsigned char *event,
    size_t event_size,
    uint64_t sequence);

static honch_status_t honch_posix_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL) {
        return honch_posix_storage_missing_client();
    }
    return honch_queue_enqueue_with_sequence(client, event, event_size, sequence);
}

static honch_status_t honch_posix_queue_clear(void *ctx)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL) {
        return honch_posix_storage_missing_client();
    }
    return honch_queue_clear(client);
}

static honch_status_t honch_posix_queue_depth(void *ctx, size_t *depth)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return honch_queue_count_pending(client, depth);
}

honch_status_t honch_posix_storage_ops_init(
    honch_storage_ops_t *ops,
    honch_posix_storage_t *ctx,
    const char *queue_directory)
{
    if (ops == NULL || ctx == NULL || honch_is_blank(queue_directory)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_posix_storage_t) {
        .client = NULL,
        .queue_directory = queue_directory
    };
    *ops = (honch_storage_ops_t) {
        .state_get = honch_posix_state_get,
        .state_set = honch_posix_state_set,
        .state_delete = honch_posix_state_delete,
        .queue_push = honch_posix_queue_push,
        .queue_clear = honch_posix_queue_clear,
        .queue_depth = honch_posix_queue_depth,
        .ctx = NULL
    };
    return HONCH_OK;
}

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

static honch_status_t honch_client_post_batch(
    honch_client_t *client,
    const unsigned char *body,
    size_t body_size,
    honch_http_result_t *result)
{
    if (client != NULL && client->transport != NULL && client->transport->post_batch != NULL) {
        honch_transport_result_t transport_result = HONCH_TRANSPORT_RETRY;
        honch_status_t status = client->transport->post_batch(
            client->transport->ctx,
            client->endpoint_url,
            client->api_key,
            body,
            body_size,
            NULL,
            &transport_result);
        switch (transport_result) {
            case HONCH_TRANSPORT_ACCEPTED:
                *result = HONCH_HTTP_OK;
                break;
            case HONCH_TRANSPORT_REJECTED:
            case HONCH_TRANSPORT_AUTH_ERROR:
                *result = HONCH_HTTP_REJECTED;
                break;
            case HONCH_TRANSPORT_RETRY:
            default:
                *result = HONCH_HTTP_RETRY;
                break;
        }
        return status;
    }

    return honch_transport_post_batch(client, body, body_size, result);
}

static honch_status_t honch_queue_enqueue_with_sequence(
    honch_client_t *client,
    const unsigned char *event,
    size_t event_size,
    uint64_t sequence)
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
        (unsigned long long)sequence,
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

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size)
{
    return honch_queue_enqueue_with_sequence(client, event, event_size, client->sequence++);
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
        status = honch_client_post_batch(client, payload.data, payload.length, &result);
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
