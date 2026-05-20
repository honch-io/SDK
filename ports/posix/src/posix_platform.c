#include "honch_internal.h"
#include "honch/posix/honch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

honch_status_t honch_size_add(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    *out = left + right;
    return HONCH_OK;
}

honch_status_t honch_size_add3(size_t first, size_t second, size_t third, size_t *out)
{
    size_t partial = 0u;
    honch_status_t status = honch_size_add(first, second, &partial);
    if (status != HONCH_OK) {
        return status;
    }
    return honch_size_add(partial, third, out);
}

honch_status_t honch_size_mul(size_t left, size_t right, size_t *out)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    *out = left * right;
    return HONCH_OK;
}

bool honch_is_blank(const char *value)
{
    if (value == NULL) {
        return true;
    }

    while (*value != '\0') {
        if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r') {
            return false;
        }
        value++;
    }
    return true;
}

char *honch_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }

    size_t length = strlen(value);
    size_t total = 0u;
    if (honch_size_add(length, 1u, &total) != HONCH_OK) {
        return NULL;
    }

    char *copy = (char *)malloc(total);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length + 1u);
    return copy;
}

void honch_free_client_fields(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

    free(client->api_key);
    free(client->endpoint_url);
    free(client->device_id);
    free(client->device_model);
    free(client->firmware_version);
    free(client->environment);
    free(client->queue_directory);
    free(client->pending_directory);
    free(client->dead_directory);
    free(client->state_directory);
    free(client->distinct_id);
    free(client->session_id);
}

honch_status_t honch_buffer_init(honch_buffer_t *buffer, size_t initial_capacity)
{
    if (initial_capacity == 0u) {
        initial_capacity = 1u;
    }

    buffer->data = (char *)malloc(initial_capacity);
    if (buffer->data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    buffer->data[0] = '\0';
    buffer->length = 0u;
    buffer->capacity = initial_capacity;
    return HONCH_OK;
}

void honch_buffer_free(honch_buffer_t *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0u;
    buffer->capacity = 0u;
}

honch_status_t honch_buffer_reserve(honch_buffer_t *buffer, size_t needed)
{
    if (needed <= buffer->capacity) {
        return HONCH_OK;
    }

    size_t next = buffer->capacity == 0u ? 64u : buffer->capacity;
    while (next < needed) {
        if (next > ((size_t)-1) / 2u) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        next *= 2u;
    }

    char *data = (char *)realloc(buffer->data, next);
    if (data == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    buffer->data = data;
    buffer->capacity = next;
    return HONCH_OK;
}

honch_status_t honch_buffer_append_n(honch_buffer_t *buffer, const char *value, size_t length)
{
    size_t needed = 0u;
    honch_status_t status = honch_size_add3(buffer->length, length, 1u, &needed);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_reserve(buffer, needed);
    if (status != HONCH_OK) {
        return status;
    }

    memcpy(buffer->data + buffer->length, value, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return HONCH_OK;
}

honch_status_t honch_buffer_append(honch_buffer_t *buffer, const char *value)
{
    return honch_buffer_append_n(buffer, value, strlen(value));
}

honch_status_t honch_buffer_appendf(honch_buffer_t *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (required < 0) {
        va_end(args);
        return HONCH_ERROR_IO;
    }

    size_t needed = 0u;
    honch_status_t status = honch_size_add3(buffer->length, (size_t)required, 1u, &needed);
    if (status != HONCH_OK) {
        va_end(args);
        return status;
    }

    status = honch_buffer_reserve(buffer, needed);
    if (status != HONCH_OK) {
        va_end(args);
        return status;
    }

    int written = vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
    va_end(args);
    if (written < 0) {
        return HONCH_ERROR_IO;
    }

    buffer->length += (size_t)written;
    return HONCH_OK;
}

uint64_t honch_now_millis(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0u;
    }

    return ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
}

static uint64_t honch_posix_now_ms(void *ctx)
{
    (void)ctx;
    return honch_now_millis();
}

static uint64_t honch_posix_uptime_ms(void *ctx)
{
    return honch_posix_now_ms(ctx);
}

static honch_status_t honch_posix_random_bytes(void *ctx, uint8_t *buffer, size_t buffer_size)
{
    (void)ctx;
    if (buffer == NULL && buffer_size > 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return HONCH_ERROR_IO;
    }

    size_t offset = 0u;
    while (offset < buffer_size) {
        ssize_t read_count = read(fd, buffer + offset, buffer_size - offset);
        if (read_count <= 0) {
            close(fd);
            return HONCH_ERROR_IO;
        }
        offset += (size_t)read_count;
    }

    close(fd);
    return HONCH_OK;
}

static honch_status_t honch_posix_lock_noop(void *ctx)
{
    (void)ctx;
    return HONCH_OK;
}

static honch_status_t honch_posix_unlock_noop(void *ctx)
{
    (void)ctx;
    return HONCH_OK;
}

static void honch_posix_log_noop(void *ctx, honch_log_level_t level, const char *message)
{
    (void)ctx;
    (void)level;
    (void)message;
}

honch_status_t honch_posix_platform_ops_init(honch_platform_ops_t *ops, honch_posix_platform_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *ops = (honch_platform_ops_t) {
        .now_ms = honch_posix_now_ms,
        .uptime_ms = honch_posix_uptime_ms,
        .random_bytes = honch_posix_random_bytes,
        .lock = honch_posix_lock_noop,
        .unlock = honch_posix_unlock_noop,
        .log = honch_posix_log_noop,
        .ctx = ctx
    };
    return HONCH_OK;
}

honch_status_t honch_random_hex(char out[33])
{
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t read_count = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (read_count != (ssize_t)sizeof(bytes)) {
            return HONCH_ERROR_IO;
        }
    } else {
        uint64_t now = honch_now_millis();
        for (size_t i = 0u; i < sizeof(bytes); i++) {
            bytes[i] = (unsigned char)((now >> ((i % 8u) * 8u)) ^ (uint64_t)i ^ (uint64_t)getpid());
        }
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(bytes); i++) {
        out[i * 2u] = hex[(bytes[i] >> 4u) & 0x0fu];
        out[(i * 2u) + 1u] = hex[bytes[i] & 0x0fu];
    }
    out[32] = '\0';
    return HONCH_OK;
}

honch_status_t honch_join_path(char **out, const char *left, const char *right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool needs_separator = left_length > 0u && left[left_length - 1u] != '/';
    size_t total = 0u;
    honch_status_t status = honch_size_add3(
        left_length,
        needs_separator ? 1u : 0u,
        right_length,
        &total);
    if (status == HONCH_OK) {
        status = honch_size_add(total, 1u, &total);
    }
    if (status != HONCH_OK) {
        return status;
    }

    char *path = (char *)malloc(total);
    if (path == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    snprintf(path, total, "%s%s%s", left, needs_separator ? "/" : "", right);
    *out = path;
    return HONCH_OK;
}

honch_status_t honch_mkdir_p(const char *path)
{
    char *copy = honch_strdup(path);
    if (copy == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
                free(copy);
                return HONCH_ERROR_IO;
            }
            *cursor = '/';
        }
    }

    if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
        free(copy);
        return HONCH_ERROR_IO;
    }

    free(copy);
    return HONCH_OK;
}

static honch_status_t honch_read_file_impl(
    const char *path,
    size_t max_bytes,
    bool enforce_limit,
    unsigned char **out,
    size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return HONCH_ERROR_IO;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return HONCH_ERROR_IO;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return HONCH_ERROR_IO;
    }
    size_t data_size = (size_t)size;
    if (enforce_limit && data_size > max_bytes) {
        fclose(file);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    size_t alloc_size = 0u;
    if (honch_size_add(data_size, 1u, &alloc_size) != HONCH_OK) {
        fclose(file);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return HONCH_ERROR_IO;
    }

    unsigned char *data = (unsigned char *)malloc(alloc_size);
    if (data == NULL) {
        fclose(file);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    size_t read_count = fread(data, 1u, data_size, file);
    fclose(file);
    if (read_count != data_size) {
        free(data);
        return HONCH_ERROR_IO;
    }

    data[data_size] = '\0';
    *out = data;
    if (out_size != NULL) {
        *out_size = data_size;
    }
    return HONCH_OK;
}

honch_status_t honch_read_file(const char *path, char **out)
{
    return honch_read_file_impl(path, 0u, false, (unsigned char **)out, NULL);
}

honch_status_t honch_read_file_limited(const char *path, size_t max_bytes, char **out)
{
    return honch_read_file_impl(path, max_bytes, true, (unsigned char **)out, NULL);
}

honch_status_t honch_read_file_limited_bytes(const char *path, size_t max_bytes, honch_payload_t *out)
{
    out->data = NULL;
    out->length = 0u;
    return honch_read_file_impl(path, max_bytes, true, &out->data, &out->length);
}

static honch_status_t honch_fsync_directory(const char *directory)
{
    int fd = open(directory, O_RDONLY);
    if (fd < 0) {
        return HONCH_ERROR_IO;
    }

    honch_status_t status = fsync(fd) == 0 ? HONCH_OK : HONCH_ERROR_IO;
    if (close(fd) != 0 && status == HONCH_OK) {
        status = HONCH_ERROR_IO;
    }
    return status;
}

honch_status_t honch_write_file_atomic_bytes_with_durability(
    const char *directory,
    const char *filename,
    const unsigned char *content,
    size_t content_size,
    honch_durability_mode_t durability_mode)
{
    char *tmp_name = NULL;
    char *tmp_path = NULL;
    char *final_path = NULL;
    honch_status_t status = HONCH_OK;

    size_t tmp_length = 0u;
    status = honch_size_add(strlen(filename), 5u, &tmp_length);
    if (status != HONCH_OK) {
        return status;
    }

    tmp_name = (char *)malloc(tmp_length);
    if (tmp_name == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }
    snprintf(tmp_name, tmp_length, "%s.tmp", filename);

    status = honch_join_path(&tmp_path, directory, tmp_name);
    if (status == HONCH_OK) {
        status = honch_join_path(&final_path, directory, filename);
    }

    if (status == HONCH_OK) {
        FILE *file = fopen(tmp_path, "wb");
        if (file == NULL) {
            status = HONCH_ERROR_IO;
        } else {
            if (fwrite(content, 1u, content_size, file) != content_size) {
                status = HONCH_ERROR_IO;
            }
            if (fflush(file) != 0) {
                status = HONCH_ERROR_IO;
            }
            int fd = fileno(file);
            if (durability_mode == HONCH_DURABILITY_SYNC_ALWAYS && fd >= 0 && fsync(fd) != 0) {
                status = HONCH_ERROR_IO;
            }
            if (fclose(file) != 0) {
                status = HONCH_ERROR_IO;
            }
        }
    }

    if (status == HONCH_OK && rename(tmp_path, final_path) != 0) {
        status = HONCH_ERROR_IO;
    }
    if (status == HONCH_OK && durability_mode == HONCH_DURABILITY_SYNC_ALWAYS) {
        status = honch_fsync_directory(directory);
    }

    if (status != HONCH_OK && tmp_path != NULL) {
        (void)unlink(tmp_path);
    }

    free(tmp_name);
    free(tmp_path);
    free(final_path);
    return status;
}

honch_status_t honch_write_file_atomic_bytes(
    const char *directory,
    const char *filename,
    const unsigned char *content,
    size_t content_size)
{
    return honch_write_file_atomic_bytes_with_durability(
        directory,
        filename,
        content,
        content_size,
        HONCH_DURABILITY_SYNC_ALWAYS);
}

honch_status_t honch_write_file_atomic(const char *directory, const char *filename, const char *content)
{
    return honch_write_file_atomic_bytes(
        directory,
        filename,
        (const unsigned char *)content,
        strlen(content));
}

static int honch_compare_entries(const void *left, const void *right)
{
    const honch_file_entry_t *a = (const honch_file_entry_t *)left;
    const honch_file_entry_t *b = (const honch_file_entry_t *)right;
    return strcmp(a->name, b->name);
}

void honch_file_list_free(honch_file_list_t *list)
{
    for (size_t i = 0u; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

static honch_status_t honch_file_list_append(honch_file_list_t *list, const char *directory, const char *name)
{
    if (list->count == list->capacity) {
        size_t next = 0u;
        honch_status_t status = HONCH_OK;
        if (list->capacity == 0u) {
            next = 16u;
        } else {
            status = honch_size_mul(list->capacity, 2u, &next);
        }
        size_t alloc_size = 0u;
        if (status == HONCH_OK) {
            status = honch_size_mul(next, sizeof(*list->items), &alloc_size);
        }
        if (status != HONCH_OK) {
            return status;
        }

        honch_file_entry_t *items = (honch_file_entry_t *)realloc(list->items, alloc_size);
        if (items == NULL) {
            return HONCH_ERROR_OUT_OF_MEMORY;
        }
        list->items = items;
        list->capacity = next;
    }

    char *entry_name = honch_strdup(name);
    if (entry_name == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    char *entry_path = NULL;
    honch_status_t status = honch_join_path(&entry_path, directory, name);
    if (status != HONCH_OK) {
        free(entry_name);
        return status;
    }

    list->items[list->count].name = entry_name;
    list->items[list->count].path = entry_path;
    list->count++;
    return HONCH_OK;
}

honch_status_t honch_list_files_with_suffix(const char *directory, const char *suffix, honch_file_list_t *list)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return errno == ENOENT ? HONCH_OK : HONCH_ERROR_IO;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        size_t name_length = strlen(entry->d_name);
        size_t suffix_length = strlen(suffix);
        if (name_length < suffix_length || strcmp(entry->d_name + name_length - suffix_length, suffix) != 0) {
            continue;
        }

        honch_status_t status = honch_file_list_append(list, directory, entry->d_name);
        if (status != HONCH_OK) {
            closedir(dir);
            return status;
        }
    }

    closedir(dir);
    qsort(list->items, list->count, sizeof(*list->items), honch_compare_entries);
    return HONCH_OK;
}

honch_status_t honch_unlink_if_exists(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT) {
        return HONCH_OK;
    }
    return HONCH_ERROR_IO;
}
