#include "honch_internal.h"
#include "honch/posix/honch.h"

#include <dirent.h>
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

/* Make a prior directory-entry change (unlink/rename) durable when the client
 * is configured for synchronous durability. Enqueue already fsyncs the pending
 * directory after writing; the removal side must do the same so an event that
 * Capture has accepted (and we unlinked) or dead-lettered cannot reappear after
 * a crash. A no-op in OS_BUFFERED mode. */
static honch_status_t honch_sync_dir_if_durable(const honch_client_t *client, const char *directory)
{
    if (client->durability_mode != HONCH_DURABILITY_SYNC_ALWAYS) {
        return HONCH_OK;
    }
    return honch_fsync_directory(directory);
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
    if (info.st_size < 0 || (size_t)info.st_size != content_size) {
        /* The NUL-terminated length differs from the on-disk byte count: the
         * file holds an embedded NUL (corruption / torn legacy write) or changed
         * under us. Refuse rather than silently persist a truncated identity. */
        free(content);
        return HONCH_ERROR_IO;
    }
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

    return honch_write_file_atomic_bytes_with_durability(
        client->state_directory,
        key,
        data,
        data_size,
        client->durability_mode);
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
static honch_status_t honch_find_queue_entry_by_sequence_scan(
    honch_client_t *client,
    uint64_t sequence,
    honch_file_entry_t *entry);
static honch_status_t honch_count_queue_files_scan(const char *directory, size_t *count);
static honch_status_t honch_delete_files_with_suffix_scan(const char *directory, const char *suffix);
static honch_status_t honch_move_to_dead(honch_client_t *client, const honch_file_entry_t *entry);

static bool honch_name_has_suffix(const char *name, const char *suffix)
{
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return name_length >= suffix_length && strcmp(name + name_length - suffix_length, suffix) == 0;
}

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

static void honch_file_entry_free(honch_file_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }
    free(entry->name);
    free(entry->path);
    entry->name = NULL;
    entry->path = NULL;
}

static honch_status_t honch_posix_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || buffer == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_entry_t entry = {0};
    honch_status_t status =
        honch_find_queue_entry_by_sequence_scan(client, client->active_storage_reader_sequence, &entry);
    if (status == HONCH_OK) {
        honch_payload_t payload = {0};
        status = honch_read_file_limited_bytes(entry.path, client->max_event_bytes, &payload);
        if (status == HONCH_OK) {
            if (offset > payload.length || buffer_size > payload.length - offset) {
                status = HONCH_ERROR_INVALID_ARGUMENT;
            } else {
                memcpy(buffer, payload.data + offset, buffer_size);
            }
        }
        free(payload.data);
    }
    honch_file_entry_free(&entry);
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

static void honch_posix_free_storage_events(honch_storage_event_t *events, size_t event_count)
{
    if (events == NULL) {
        return;
    }
    for (size_t i = 0u; i < event_count; i++) {
        free(events[i].data);
        events[i].data = NULL;
        events[i].length = 0u;
        events[i].sequence = 0u;
    }
}

static honch_status_t honch_posix_queue_read_batch(
    void *ctx,
    honch_storage_event_t *events,
    size_t max_events,
    size_t max_event_bytes,
    size_t *event_count)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || events == NULL || event_count == NULL || max_events == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *event_count = 0u;
    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    if (status != HONCH_OK) {
        honch_file_list_free(&files);
        return status;
    }

    for (size_t i = 0u; status == HONCH_OK && i < files.count && *event_count < max_events; i++) {
        uint64_t sequence = 0u;
        if (!honch_queue_sequence_from_name(files.items[i].name, &sequence)) {
            continue;
        }

        honch_payload_t payload = {0};
        status = honch_read_file_limited_bytes(files.items[i].path, max_event_bytes, &payload);
        if (status != HONCH_OK) {
            break;
        }
        if (payload.data == NULL || payload.length > max_event_bytes) {
            free(payload.data);
            status = HONCH_ERROR_INVALID_ARGUMENT;
            break;
        }

        events[*event_count] = (honch_storage_event_t) {
            .data = payload.data,
            .length = payload.length,
            .sequence = sequence
        };
        (*event_count)++;
    }

    honch_file_list_free(&files);
    if (status != HONCH_OK) {
        honch_posix_free_storage_events(events, *event_count);
        *event_count = 0u;
        return status;
    }
    return *event_count == 0u ? HONCH_ERROR_NOT_INITIALIZED : HONCH_OK;
}

static honch_status_t honch_posix_queue_consume(void *ctx, uint64_t sequence)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_file_entry_t entry = {0};
    honch_status_t status = honch_find_queue_entry_by_sequence_scan(client, sequence, &entry);
    if (status == HONCH_OK) {
        status = honch_unlink_if_exists(entry.path);
    }
    if (status == HONCH_OK && client->queued_event_count > 0u) {
        client->queued_event_count--;
    }
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->pending_directory);
    }
    honch_file_entry_free(&entry);
    return status;
}

static honch_status_t honch_posix_queue_consume_batch(
    void *ctx,
    const uint64_t *sequences,
    size_t sequence_count)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || (sequences == NULL && sequence_count > 0u)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        honch_file_entry_t *entry = honch_find_queue_entry_by_sequence(&files, sequences[i]);
        if (entry == NULL) {
            status = HONCH_ERROR_NOT_INITIALIZED;
            break;
        }
        status = honch_unlink_if_exists(entry->path);
        if (status == HONCH_OK && client->queued_event_count > 0u) {
            client->queued_event_count--;
        }
    }
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->pending_directory);
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

    honch_file_entry_t entry = {0};
    honch_status_t status = honch_find_queue_entry_by_sequence_scan(client, sequence, &entry);
    if (status == HONCH_OK) {
        status = honch_move_to_dead(client, &entry);
    }
    if (status == HONCH_OK && client->queued_event_count > 0u) {
        client->queued_event_count--;
    }
    /* The rename changed both directory entries: make both durable. */
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->dead_directory);
    }
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->pending_directory);
    }
    honch_file_entry_free(&entry);
    return status;
}

static honch_status_t honch_posix_queue_dead_letter_batch(
    void *ctx,
    const uint64_t *sequences,
    size_t sequence_count)
{
    honch_client_t *client = (honch_client_t *)ctx;
    if (client == NULL || (sequences == NULL && sequence_count > 0u)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (sequence_count == 0u) {
        return HONCH_OK;
    }

    honch_file_list_t files = {0};
    honch_status_t status = honch_list_queue_files(client, &files);
    for (size_t i = 0u; status == HONCH_OK && i < sequence_count; i++) {
        honch_file_entry_t *entry = honch_find_queue_entry_by_sequence(&files, sequences[i]);
        if (entry == NULL) {
            status = HONCH_ERROR_NOT_INITIALIZED;
            break;
        }
        status = honch_move_to_dead(client, entry);
        if (status == HONCH_OK && client->queued_event_count > 0u) {
            client->queued_event_count--;
        }
    }
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->dead_directory);
    }
    if (status == HONCH_OK) {
        status = honch_sync_dir_if_durable(client, client->pending_directory);
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
    honch_state_storage_ops_t *state_ops,
    honch_event_queue_ops_t *queue_ops,
    honch_posix_storage_t *ctx,
    const char *queue_directory)
{
    if (state_ops == NULL || queue_ops == NULL || ctx == NULL || honch_is_blank(queue_directory)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_posix_storage_t) {
        .client = NULL,
        .queue_directory = queue_directory
    };
    *state_ops = (honch_state_storage_ops_t) {
        .state_get = honch_posix_state_get,
        .state_set = honch_posix_state_set,
        .state_delete = honch_posix_state_delete,
        .ctx = NULL
    };
    *queue_ops = (honch_event_queue_ops_t) {
        .queue_push = honch_posix_queue_push,
        .queue_peek = honch_posix_queue_peek,
        .queue_read_batch = honch_posix_queue_read_batch,
        .queue_consume = honch_posix_queue_consume,
        .queue_consume_batch = honch_posix_queue_consume_batch,
        .queue_dead_letter = honch_posix_queue_dead_letter,
        .queue_dead_letter_batch = honch_posix_queue_dead_letter_batch,
        .queue_clear = honch_posix_queue_clear,
        .queue_depth = honch_posix_queue_depth,
        .ctx = NULL
    };
    return HONCH_OK;
}

static honch_status_t honch_list_queue_files(honch_client_t *client, honch_file_list_t *files)
{
    honch_status_t status = honch_list_files_with_suffix(client->pending_directory, ".hqe", files);
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

static honch_status_t honch_find_queue_entry_by_sequence_scan(
    honch_client_t *client,
    uint64_t sequence,
    honch_file_entry_t *entry)
{
    if (client == NULL || entry == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *entry = (honch_file_entry_t){0};
    DIR *dir = opendir(client->pending_directory);
    if (dir == NULL) {
        return errno == ENOENT ? HONCH_ERROR_NOT_INITIALIZED : HONCH_ERROR_IO;
    }

    honch_status_t status = HONCH_ERROR_NOT_INITIALIZED;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir)) != NULL) {
        if (dir_entry->d_name[0] == '.' || !honch_name_has_suffix(dir_entry->d_name, ".hqe")) {
            continue;
        }

        uint64_t entry_sequence = 0u;
        if (!honch_queue_sequence_from_name(dir_entry->d_name, &entry_sequence) ||
            entry_sequence != sequence) {
            continue;
        }

        entry->name = honch_strdup(dir_entry->d_name);
        if (entry->name == NULL) {
            status = HONCH_ERROR_OUT_OF_MEMORY;
            break;
        }
        status = honch_join_path(&entry->path, client->pending_directory, dir_entry->d_name);
        if (status != HONCH_OK) {
            honch_file_entry_free(entry);
        }
        break;
    }

    if (closedir(dir) != 0 && status == HONCH_ERROR_NOT_INITIALIZED) {
        status = HONCH_ERROR_IO;
    }
    return status;
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
        /*
         * Evict the lowest-sequence (true oldest) entries. The file list is
         * sorted lexically by name, but filenames are <timestamp>-<sequence>-...
         * and the sequence field is not fixed-width, so lexical order diverges
         * from sequence order once sequences exceed the padded width or the clock
         * is non-monotonic. Selecting the minimum parsed sequence each round
         * ensures drop-oldest never discards a newer event while an older one
         * remains. An unparseable name is treated as oldest so the queue still
         * makes progress.
         */
        while (client->queued_event_count >= client->max_queued_events) {
            size_t victim = files.count;
            uint64_t victim_sequence = UINT64_MAX;
            for (size_t i = 0u; i < files.count; i++) {
                if (files.items[i].path == NULL) {
                    continue; /* already evicted this round */
                }
                uint64_t sequence = 0u;
                if (!honch_queue_sequence_from_name(files.items[i].name, &sequence)) {
                    victim = i;
                    break;
                }
                if (victim == files.count || sequence < victim_sequence) {
                    victim = i;
                    victim_sequence = sequence;
                }
            }
            if (victim == files.count) {
                break; /* nothing left to evict */
            }
            status = honch_unlink_if_exists(files.items[victim].path);
            if (status != HONCH_OK) {
                honch_file_list_free(&files);
                return status;
            }
            free(files.items[victim].path);
            files.items[victim].path = NULL; /* mark evicted */
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
        "%020llu-%06llu-%s.hqe",
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
    if (client == NULL || client->sequence == UINT64_MAX) {
        return HONCH_ERROR_QUEUE_FULL;
    }

    return honch_queue_enqueue_with_sequence(client, event, event_size, client->sequence++);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count)
{
    if (client == NULL || count == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return honch_count_queue_files_scan(client->pending_directory, count);
}

static honch_status_t honch_count_queue_files_scan(const char *directory, size_t *count)
{
    if (directory == NULL || count == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *count = 0u;
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return errno == ENOENT ? HONCH_OK : HONCH_ERROR_IO;
    }

    honch_status_t status = HONCH_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !honch_name_has_suffix(entry->d_name, ".hqe")) {
            continue;
        }

        uint64_t sequence = 0u;
        if (honch_queue_sequence_from_name(entry->d_name, &sequence)) {
            (*count)++;
        }
    }

    if (closedir(dir) != 0) {
        status = HONCH_ERROR_IO;
    }
    return status;
}

static honch_status_t honch_delete_files_with_suffix_scan(const char *directory, const char *suffix)
{
    if (directory == NULL || suffix == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return errno == ENOENT ? HONCH_OK : HONCH_ERROR_IO;
    }

    honch_status_t status = HONCH_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !honch_name_has_suffix(entry->d_name, suffix)) {
            continue;
        }

        char *path = NULL;
        status = honch_join_path(&path, directory, entry->d_name);
        if (status != HONCH_OK) {
            break;
        }
        status = honch_unlink_if_exists(path);
        free(path);
        if (status != HONCH_OK) {
            break;
        }
    }

    if (closedir(dir) != 0 && status == HONCH_OK) {
        status = HONCH_ERROR_IO;
    }
    return status;
}

static honch_status_t honch_delete_queue_directory(const char *directory)
{
    return honch_delete_files_with_suffix_scan(directory, ".hqe");
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
