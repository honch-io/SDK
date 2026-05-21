#include "honch_micropython.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static honch_status_t honch_mp_import_attr(qstr module_name, qstr attr_name, mp_obj_t *out)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    mp_obj_t module = mp_import_name(module_name, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    *out = mp_load_attr(module, attr_name);
    nlr_pop();
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_call_os1(qstr attr_name, const char *arg)
{
    mp_obj_t fn = mp_const_none;
    honch_status_t status = honch_mp_import_attr(MP_QSTR_os, attr_name, &fn);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    mp_call_function_1(fn, mp_obj_new_str(arg, strlen(arg)));
    nlr_pop();
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_mkdir_p(const char *path)
{
    if (path == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *copy = honch_micropython_strdup(path);
    if (copy == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            (void)honch_mp_call_os1(MP_QSTR_mkdir, copy);
            *cursor = '/';
        }
    }
    (void)honch_mp_call_os1(MP_QSTR_mkdir, copy);
    free(copy);
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_read_file(const char *path, mp_obj_t *data)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    mp_obj_t open_args[2] = {
        mp_obj_new_str(path, strlen(path)),
        mp_obj_new_str("rb", 2),
    };
    mp_obj_t file = mp_builtin_open(2, open_args, (mp_map_t *)&mp_const_empty_map);
    mp_obj_t read = mp_load_attr(file, MP_QSTR_read);
    *data = mp_call_function_0(read);
    (void)honch_micropython_call_noarg_attr(file, MP_QSTR_close);
    nlr_pop();
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_write_file(const char *path, const uint8_t *data, size_t data_size)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    mp_obj_t open_args[2] = {
        mp_obj_new_str(path, strlen(path)),
        mp_obj_new_str("wb", 2),
    };
    mp_obj_t file = mp_builtin_open(2, open_args, (mp_map_t *)&mp_const_empty_map);
    mp_obj_t write = mp_load_attr(file, MP_QSTR_write);
    mp_call_function_1(write, mp_obj_new_bytes(data, data_size));
    (void)honch_micropython_call_noarg_attr(file, MP_QSTR_close);
    nlr_pop();
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_storage_path(char **out, const char *directory, const char *name)
{
    return honch_micropython_join_path(out, directory, name);
}

static honch_status_t honch_mp_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || key == NULL || buffer_size == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *path = NULL;
    honch_status_t status = honch_mp_storage_path(&path, storage->state_directory, key);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    mp_obj_t data = mp_const_none;
    status = honch_mp_read_file(path, &data);
    free(path);
    if (status != HONCH_STATUS_OK) {
        *buffer_size = 0u;
        return HONCH_STATUS_OK;
    }
    size_t data_size = 0u;
    const char *data_ptr = mp_obj_str_get_data(data, &data_size);
    if (buffer == NULL || *buffer_size < data_size) {
        *buffer_size = data_size;
        return buffer == NULL ? HONCH_STATUS_OK : HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    memcpy(buffer, data_ptr, data_size);
    *buffer_size = data_size;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_state_set(void *ctx, const char *key, const uint8_t *data, size_t data_size)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || key == NULL || (data == NULL && data_size > 0u)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *path = NULL;
    honch_status_t status = honch_mp_storage_path(&path, storage->state_directory, key);
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_write_file(path, data, data_size);
    }
    free(path);
    return status;
}

static honch_status_t honch_mp_state_delete(void *ctx, const char *key)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || key == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *path = NULL;
    honch_status_t status = honch_mp_storage_path(&path, storage->state_directory, key);
    if (status == HONCH_STATUS_OK) {
        (void)honch_mp_call_os1(MP_QSTR_remove, path);
    }
    free(path);
    return status;
}

static void honch_mp_sequence_name(uint64_t sequence, char out[32])
{
    snprintf(out, 32u, "%020llu.cbor", (unsigned long long)sequence);
}

static int honch_mp_parse_sequence(const char *name, uint64_t *sequence)
{
    if (name == NULL || strlen(name) != 25u || strcmp(name + 20u, ".cbor") != 0) {
        return 0;
    }
    uint64_t value = 0u;
    for (size_t i = 0u; i < 20u; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        value = (value * 10u) + (uint64_t)(name[i] - '0');
    }
    *sequence = value;
    return 1;
}

static honch_status_t honch_mp_listdir(const char *directory, mp_obj_t *items)
{
    mp_obj_t listdir = mp_const_none;
    honch_status_t status = honch_mp_import_attr(MP_QSTR_os, MP_QSTR_listdir, &listdir);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) != 0) {
        return HONCH_STATUS_ERROR_IO;
    }
    *items = mp_call_function_1(listdir, mp_obj_new_str(directory, strlen(directory)));
    nlr_pop();
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_find_oldest(
    honch_micropython_storage_t *storage,
    uint64_t after,
    char **name_out,
    uint64_t *sequence_out)
{
    mp_obj_t items = mp_const_none;
    honch_status_t status = honch_mp_listdir(storage->pending_directory, &items);
    if (status != HONCH_STATUS_OK) {
        return HONCH_STATUS_ERROR_NOT_INITIALIZED;
    }
    size_t count = 0u;
    mp_obj_t *entries = NULL;
    mp_obj_get_array(items, &count, &entries);
    char *best_name = NULL;
    uint64_t best_sequence = 0u;
    int found = 0;
    for (size_t i = 0u; i < count; i++) {
        size_t name_size = 0u;
        const char *name = mp_obj_str_get_data(entries[i], &name_size);
        uint64_t sequence = 0u;
        if (!honch_mp_parse_sequence(name, &sequence) ||
            (after != UINT64_MAX && sequence <= after)) {
            continue;
        }
        if (!found || sequence < best_sequence) {
            char *copy = honch_micropython_strdup(name);
            if (copy == NULL) {
                free(best_name);
                return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
            }
            free(best_name);
            best_name = copy;
            best_sequence = sequence;
            found = 1;
        }
    }
    if (!found) {
        return HONCH_STATUS_ERROR_NOT_INITIALIZED;
    }
    *name_out = best_name;
    *sequence_out = best_sequence;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || event == NULL || event_size == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char name[32];
    honch_mp_sequence_name(sequence, name);
    char *path = NULL;
    honch_status_t status = honch_mp_storage_path(&path, storage->pending_directory, name);
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_write_file(path, event, event_size);
    }
    free(path);
    return status;
}

static honch_status_t honch_mp_queue_file_path(honch_micropython_storage_t *storage, uint64_t sequence, char **out)
{
    char name[32];
    honch_mp_sequence_name(sequence, name);
    return honch_mp_storage_path(out, storage->pending_directory, name);
}

static honch_status_t honch_mp_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || buffer == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *path = NULL;
    honch_status_t status = honch_mp_queue_file_path(storage, storage->active_sequence, &path);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    mp_obj_t data = mp_const_none;
    status = honch_mp_read_file(path, &data);
    free(path);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    size_t data_size = 0u;
    const char *data_ptr = mp_obj_str_get_data(data, &data_size);
    if (offset > data_size || buffer_size > data_size - offset) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    memcpy(buffer, data_ptr + offset, buffer_size);
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || reader == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char *name = NULL;
    uint64_t sequence = 0u;
    honch_status_t status = honch_mp_find_oldest(storage, storage->active_sequence, &name, &sequence);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    char *path = NULL;
    status = honch_mp_storage_path(&path, storage->pending_directory, name);
    free(name);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    mp_obj_t data = mp_const_none;
    status = honch_mp_read_file(path, &data);
    free(path);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    size_t data_size = 0u;
    (void)mp_obj_str_get_data(data, &data_size);
    storage->active_sequence = sequence;
    *reader = (honch_storage_reader_t) {
        .ctx = storage,
        .read = honch_mp_reader_read,
        .total_size = data_size,
        .sequence = sequence,
    };
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_remove_pending(honch_micropython_storage_t *storage, uint64_t sequence)
{
    char *path = NULL;
    honch_status_t status = honch_mp_queue_file_path(storage, sequence, &path);
    if (status == HONCH_STATUS_OK) {
        (void)honch_mp_call_os1(MP_QSTR_remove, path);
    }
    free(path);
    return status;
}

static honch_status_t honch_mp_queue_consume(void *ctx, uint64_t sequence)
{
    return honch_mp_remove_pending((honch_micropython_storage_t *)ctx, sequence);
}

static honch_status_t honch_mp_queue_dead_letter(void *ctx, uint64_t sequence)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    char name[32];
    honch_mp_sequence_name(sequence, name);
    char *src = NULL;
    char *dst = NULL;
    honch_status_t status = honch_mp_storage_path(&src, storage->pending_directory, name);
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_storage_path(&dst, storage->dead_directory, name);
    }
    if (status == HONCH_STATUS_OK) {
        mp_obj_t rename_fn = mp_const_none;
        status = honch_mp_import_attr(MP_QSTR_os, MP_QSTR_rename, &rename_fn);
        if (status == HONCH_STATUS_OK) {
            nlr_buf_t nlr;
            if (nlr_push(&nlr) != 0) {
                status = HONCH_STATUS_ERROR_IO;
            } else {
                mp_call_function_2(rename_fn, mp_obj_new_str(src, strlen(src)), mp_obj_new_str(dst, strlen(dst)));
                nlr_pop();
            }
        }
    }
    free(src);
    free(dst);
    return status;
}

static honch_status_t honch_mp_queue_drop_oldest(void *ctx)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    char *name = NULL;
    uint64_t sequence = 0u;
    honch_status_t status = honch_mp_find_oldest(storage, UINT64_MAX, &name, &sequence);
    free(name);
    if (status == HONCH_STATUS_ERROR_NOT_INITIALIZED) {
        return HONCH_STATUS_OK;
    }
    return status == HONCH_STATUS_OK ? honch_mp_remove_pending(storage, sequence) : status;
}

static honch_status_t honch_mp_clear_dir(const char *directory)
{
    mp_obj_t items = mp_const_none;
    honch_status_t status = honch_mp_listdir(directory, &items);
    if (status != HONCH_STATUS_OK) {
        return HONCH_STATUS_OK;
    }
    size_t count = 0u;
    mp_obj_t *entries = NULL;
    mp_obj_get_array(items, &count, &entries);
    for (size_t i = 0u; i < count; i++) {
        size_t name_size = 0u;
        const char *name = mp_obj_str_get_data(entries[i], &name_size);
        uint64_t ignored = 0u;
        if (!honch_mp_parse_sequence(name, &ignored)) {
            continue;
        }
        char *path = NULL;
        if (honch_mp_storage_path(&path, directory, name) == HONCH_STATUS_OK) {
            (void)honch_mp_call_os1(MP_QSTR_remove, path);
        }
        free(path);
    }
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_queue_clear(void *ctx)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    (void)honch_mp_clear_dir(storage->pending_directory);
    (void)honch_mp_clear_dir(storage->dead_directory);
    storage->active_sequence = UINT64_MAX;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_mp_queue_depth(void *ctx, size_t *depth)
{
    honch_micropython_storage_t *storage = (honch_micropython_storage_t *)ctx;
    if (storage == NULL || depth == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *depth = 0u;
    mp_obj_t items = mp_const_none;
    honch_status_t status = honch_mp_listdir(storage->pending_directory, &items);
    if (status != HONCH_STATUS_OK) {
        return HONCH_STATUS_OK;
    }
    size_t count = 0u;
    mp_obj_t *entries = NULL;
    mp_obj_get_array(items, &count, &entries);
    for (size_t i = 0u; i < count; i++) {
        size_t name_size = 0u;
        const char *name = mp_obj_str_get_data(entries[i], &name_size);
        uint64_t ignored = 0u;
        if (honch_mp_parse_sequence(name, &ignored)) {
            (*depth)++;
        }
    }
    return HONCH_STATUS_OK;
}

honch_status_t honch_micropython_storage_ops_init(
    honch_storage_ops_t *ops,
    honch_micropython_storage_t *ctx,
    const char *queue_directory)
{
    HONCH_MP_DEBUG_INIT("storage_init_begin");
    if (ops == NULL || ctx == NULL || queue_directory == NULL || queue_directory[0] == '\0') {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    *ctx = (honch_micropython_storage_t) {
        .queue_directory = honch_micropython_strdup(queue_directory),
        .active_sequence = UINT64_MAX,
    };
    if (ctx->queue_directory == NULL) {
        return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
    }
    honch_status_t status = honch_mp_storage_path(&ctx->pending_directory, ctx->queue_directory, "pending");
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_storage_path(&ctx->dead_directory, ctx->queue_directory, "dead");
    }
    if (status == HONCH_STATUS_OK) {
        status = honch_mp_storage_path(&ctx->state_directory, ctx->queue_directory, "state");
    }
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("storage_mkdir_pending");
        status = honch_mp_mkdir_p(ctx->pending_directory);
    }
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("storage_mkdir_dead");
        status = honch_mp_mkdir_p(ctx->dead_directory);
    }
    if (status == HONCH_STATUS_OK) {
        HONCH_MP_DEBUG_INIT("storage_mkdir_state");
        status = honch_mp_mkdir_p(ctx->state_directory);
    }
    if (status != HONCH_STATUS_OK) {
        honch_micropython_storage_ops_deinit(ctx);
        return status;
    }
    *ops = (honch_storage_ops_t) {
        .state_get = honch_mp_state_get,
        .state_set = honch_mp_state_set,
        .state_delete = honch_mp_state_delete,
        .queue_push = honch_mp_queue_push,
        .queue_peek = honch_mp_queue_peek,
        .queue_consume = honch_mp_queue_consume,
        .queue_dead_letter = honch_mp_queue_dead_letter,
        .queue_drop_oldest = honch_mp_queue_drop_oldest,
        .queue_clear = honch_mp_queue_clear,
        .queue_depth = honch_mp_queue_depth,
        .ctx = ctx,
    };
    HONCH_MP_DEBUG_INIT("storage_init_done");
    return HONCH_STATUS_OK;
}

void honch_micropython_storage_ops_deinit(honch_micropython_storage_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->queue_directory);
    free(ctx->pending_directory);
    free(ctx->dead_directory);
    free(ctx->state_directory);
    *ctx = (honch_micropython_storage_t) {0};
}
