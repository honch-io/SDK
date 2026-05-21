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
static honch_status_t honch_list_queue_files(honch_client_t *client, honch_file_list_t *files);
static honch_status_t honch_move_to_dead(honch_client_t *client, const honch_file_entry_t *entry);

static bool honch_queue_sequence_from_name(const char *name, uint64_t *sequence)
{
    const char *first_dash = strchr(name, '-');
    if (first_dash == NULL) {
        *sequence = 0u;
        return false;
    }

    const char *cursor = first_dash + 1;
    if (*cursor == '\0') {
        *sequence = 0u;
        return false;
    }

    uint64_t value = 0u;
    bool saw_digit = false;
    while (*cursor >= '0' && *cursor <= '9') {
        saw_digit = true;
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            *sequence = 0u;
            return false;
        }
        value = (value * 10u) + digit;
        cursor++;
    }

    if (!saw_digit || *cursor != '-') {
        *sequence = 0u;
        return false;
    }

    *sequence = value;
    return true;
}

static honch_file_entry_t *honch_find_queue_entry_by_sequence(honch_file_list_t *files, uint64_t sequence)
{
    for (size_t i = 0u; i < files->count; i++) {
        uint64_t entry_sequence = 0u;
        if (honch_queue_sequence_from_name(files->items[i].name, &entry_sequence) &&
            entry_sequence == sequence) {
            return &files->items[i];
        }
    }
    return NULL;
}

static honch_status_t honch_posix_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || buffer == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    honch_file_entry_t *entry = status == HONCH_OK ?
        honch_find_queue_entry_by_sequence(&files, client->active_storage_reader_sequence) :
        NULL;
    if (status == HONCH_OK && entry == NULL) {
        status = HONCH_ERROR_NOT_INITIALIZED;
    }
    if (status == HONCH_OK) {
        honch_payload_t payload = {0};
        status = honch_read_file_limited_bytes(entry->path, client->max_event_bytes, &payload);
        if (status == HONCH_OK) {
            if (offset > payload.length || buffer_size > payload.length - offset) {
                status = HONCH_ERROR_INVALID_ARGUMENT;
            } else {
                memcpy(buffer, payload.data + offset, buffer_size);
            }
        }
        free(payload.data);
    }
    honch_file_list_free(&files);
    return status;
}

static honch_status_t honch_posix_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || reader == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    if (status != HONCH_OK) {
        honch_file_list_free(&files);
        return status;
    }
    if (files.count == 0u) {
        honch_file_list_free(&files);
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    size_t selected = 0u;
    if (client->active_storage_reader_sequence != UINT64_MAX) {
        selected = files.count;
        for (size_t i = 0u; i < files.count; i++) {
            uint64_t entry_sequence = 0u;
            if (honch_queue_sequence_from_name(files.items[i].name, &entry_sequence) &&
                entry_sequence == client->active_storage_reader_sequence) {
                selected = i + 1u;
                break;
            }
        }
        if (selected >= files.count) {
            honch_file_list_free(&files);
            return HONCH_ERROR_NOT_INITIALIZED;
        }
    }

    struct stat info;
    if (stat(files.items[selected].path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        honch_file_list_free(&files);
        return HONCH_ERROR_IO;
    }

    uint64_t sequence = 0u;
    if (!honch_queue_sequence_from_name(files.items[selected].name, &sequence)) {
        honch_file_list_free(&files);
        return HONCH_ERROR_NOT_INITIALIZED;
    }
    client->active_storage_reader_sequence = sequence;
    *reader = (honch_storage_reader_t) {
        .ctx = client,
        .read = honch_posix_reader_read,
        .total_size = (size_t)info.st_size,
        .sequence = sequence
    };

    honch_file_list_free(&files);
    return HONCH_OK;
}

static honch_status_t honch_posix_queue_consume(void *ctx, uint64_t sequence)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    honch_file_entry_t *entry = status == HONCH_OK ? honch_find_queue_entry_by_sequence(&files, sequence) : NULL;
    if (status == HONCH_OK && entry == NULL) {
        status = HONCH_ERROR_NOT_INITIALIZED;
    }
    if (status == HONCH_OK) {
        status = honch_unlink_if_exists(entry->path);
    }
    if (status == HONCH_OK && client->queued_event_count > 0u) {
        client->queued_event_count--;
    }
    honch_file_list_free(&files);
    return status;
}

static honch_status_t honch_posix_queue_dead_letter(void *ctx, uint64_t sequence)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    honch_file_entry_t *entry = status == HONCH_OK ? honch_find_queue_entry_by_sequence(&files, sequence) : NULL;
    if (status == HONCH_OK && entry == NULL) {
        status = HONCH_ERROR_NOT_INITIALIZED;
    }
    if (status == HONCH_OK) {
        status = honch_move_to_dead(client, entry);
    }
    if (status == HONCH_OK && client->queued_event_count > 0u) {
        client->queued_event_count--;
    }
    honch_file_list_free(&files);
    return status;
}

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
        .queue_peek = honch_posix_queue_peek,
        .queue_consume = honch_posix_queue_consume,
        .queue_dead_letter = honch_posix_queue_dead_letter,
        .queue_clear = honch_posix_queue_clear,
        .queue_depth = honch_posix_queue_depth,
        .ctx = NULL
    };
    return HONCH_OK;
}

static honch_status_t honch_list_queue_files(honch_client_t *client, honch_file_list_t *files)
{
    honch_status_t status = honch_list_files_with_suffix(client->pending_directory, ".cbor", files);
    if (status != HONCH_OK) {
        return status;
    }

    size_t write_index = 0u;
    for (size_t read_index = 0u; read_index < files->count; read_index++) {
        uint64_t sequence = 0u;
        if (honch_queue_sequence_from_name(files->items[read_index].name, &sequence)) {
            if (write_index != read_index) {
                files->items[write_index] = files->items[read_index];
            }
            write_index++;
        } else {
            free(files->items[read_index].name);
            free(files->items[read_index].path);
        }
    }
    files->count = write_index;
    return HONCH_OK;
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
