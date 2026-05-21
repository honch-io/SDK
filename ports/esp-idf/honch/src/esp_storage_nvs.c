// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_core_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#define HONCH_ESP_STATE_NAMESPACE "honch_state"
#define HONCH_ESP_QUEUE_NAMESPACE "honch_q"
#define HONCH_ESP_DEAD_NAMESPACE "honch_dead"
#define HONCH_ESP_NVS_KEY_SIZE 16
#define HONCH_ESP_DEFAULT_MAX_QUEUE_DEPTH 256u

static honch_status_t honch_esp_queue_depth(void *ctx, size_t *depth);
static bool honch_esp_queue_blob_exists(nvs_handle_t handle, uint64_t sequence, size_t *value_size);

static honch_status_t honch_esp_nvs_status(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
        case ESP_ERR_NVS_NOT_FOUND:
            return HONCH_STATUS_OK;
        case ESP_ERR_NO_MEM:
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_NVS_INVALID_LENGTH:
            return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
        default:
            return HONCH_STATUS_ERROR_IO;
    }
}

static void honch_esp_sequence_key(uint64_t sequence, char key[HONCH_ESP_NVS_KEY_SIZE])
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char digits[14];
    size_t count = 0u;

    do {
        digits[count++] = alphabet[sequence % 36u];
        sequence /= 36u;
    } while (sequence > 0u && count < sizeof(digits));

    key[0] = 'q';
    for (size_t i = 0u; i < count; i++) {
        key[i + 1u] = digits[count - i - 1u];
    }
    key[count + 1u] = '\0';
}

static const char *honch_esp_state_key(const char *key)
{
    if (strcmp(key, "firmware_version") == 0) {
        return "fw_ver";
    }
    return key;
}

static honch_status_t honch_esp_get_counter(const char *name, uint64_t *value)
{
    *value = 0u;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_STATUS_OK;
    }
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    err = nvs_get_u64(handle, name, value);
    nvs_close(handle);
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_set_counters(nvs_handle_t handle, uint64_t head, uint64_t tail)
{
    esp_err_t err = nvs_set_u64(handle, "head", head);
    if (err == ESP_OK) {
        err = nvs_set_u64(handle, "tail", tail);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_get_head_tail(uint64_t *head, uint64_t *tail)
{
    honch_status_t status = honch_esp_get_counter("head", head);
    if (status == HONCH_STATUS_OK) {
        status = honch_esp_get_counter("tail", tail);
    }
    if (status == HONCH_STATUS_OK && *tail > *head) {
        *tail = *head;
    }
    return status;
}

static honch_status_t honch_esp_nvs_queue_depth(size_t *depth)
{
    if (depth == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *depth = 0u;
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_STATUS_OK;
    }
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    for (uint64_t sequence = tail; sequence < head; sequence++) {
        if (honch_esp_queue_blob_exists(handle, sequence, NULL)) {
            (*depth)++;
        }
    }

    nvs_close(handle);
    return HONCH_STATUS_OK;
}

static bool honch_esp_queue_blob_exists(nvs_handle_t handle, uint64_t sequence, size_t *value_size)
{
    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(sequence, key);

    size_t size = 0u;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &size);
    if (err != ESP_OK || size == 0u) {
        return false;
    }

    if (value_size != NULL) {
        *value_size = size;
    }
    return true;
}

static honch_status_t honch_esp_advance_tail_locked(nvs_handle_t handle, uint64_t head, uint64_t *tail)
{
    while (*tail < head && !honch_esp_queue_blob_exists(handle, *tail, NULL)) {
        (*tail)++;
    }
    return honch_esp_set_counters(handle, head, *tail);
}

static honch_status_t honch_esp_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size)
{
    (void)ctx;
    if (key == NULL || buffer_size == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *buffer_size = 0u;
        return HONCH_STATUS_OK;
    }
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    size_t value_size = *buffer_size;
    const char *nvs_key = honch_esp_state_key(key);
    err = nvs_get_blob(handle, nvs_key, buffer, &value_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *buffer_size = 0u;
        return HONCH_STATUS_OK;
    }

    *buffer_size = value_size;
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_state_set(void *ctx, const char *key, const uint8_t *data, size_t data_size)
{
    (void)ctx;
    if (key == NULL || (data == NULL && data_size > 0u)) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        const char *nvs_key = honch_esp_state_key(key);
        err = nvs_set_blob(handle, nvs_key, data, data_size);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
        nvs_close(handle);
    } else if (handle != 0) {
        nvs_close(handle);
    }
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_state_delete(void *ctx, const char *key)
{
    (void)ctx;
    if (key == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_STATUS_OK;
    }
    if (err == ESP_OK) {
        const char *nvs_key = honch_esp_state_key(key);
        err = nvs_erase_key(handle, nvs_key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return honch_esp_nvs_status(err);
}

static bool honch_esp_ram_queue_has_events(const honch_esp_storage_t *storage)
{
    return storage != NULL && storage->ram_count > 0u;
}

static void honch_esp_ram_queue_reset(honch_esp_storage_t *storage)
{
    if (storage == NULL) {
        return;
    }

    storage->ram_used = 0u;
    storage->ram_count = 0u;
    storage->peek_sequence = UINT64_MAX;
    storage->read_sequence = UINT64_MAX;
}

static uintptr_t honch_esp_align_up(uintptr_t value, uintptr_t alignment)
{
    uintptr_t remainder = value % alignment;
    if (remainder == 0u) {
        return value;
    }
    return value + alignment - remainder;
}

static honch_esp_ram_queue_entry_t *honch_esp_ram_queue_find(
    honch_esp_storage_t *storage,
    uint64_t sequence)
{
    if (storage == NULL) {
        return NULL;
    }

    for (size_t i = 0u; i < storage->ram_count; i++) {
        if (storage->ram_entries[i].sequence == sequence) {
            return &storage->ram_entries[i];
        }
    }
    return NULL;
}

static honch_status_t honch_esp_ram_reader_read(
    void *ctx,
    uint32_t offset,
    uint8_t *buffer,
    size_t buffer_size)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (storage == NULL || buffer == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    honch_esp_ram_queue_entry_t *entry = honch_esp_ram_queue_find(storage, storage->read_sequence);
    if (entry == NULL || offset > entry->size || buffer_size > entry->size - offset) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    memcpy(buffer, storage->ram_buffer + entry->offset + offset, buffer_size);
    return HONCH_STATUS_OK;
}

static honch_status_t honch_esp_ram_queue_push(
    honch_esp_storage_t *storage,
    const uint8_t *event,
    size_t event_size,
    uint64_t sequence)
{
    if (storage == NULL || event == NULL || event_size == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (storage->ram_buffer == NULL || storage->ram_entries == NULL ||
        storage->nvs_fallback_active ||
        storage->ram_count >= storage->ram_entry_capacity ||
        storage->ram_used > storage->ram_buffer_size ||
        event_size > storage->ram_buffer_size - storage->ram_used ||
        event_size > UINT32_MAX || storage->ram_used > UINT32_MAX) {
        return HONCH_STATUS_ERROR_QUEUE_FULL;
    }

    memcpy(storage->ram_buffer + storage->ram_used, event, event_size);
    storage->ram_entries[storage->ram_count++] = (honch_esp_ram_queue_entry_t) {
        .sequence = sequence,
        .offset = (uint32_t)storage->ram_used,
        .size = (uint32_t)event_size
    };
    storage->ram_used += event_size;
    return HONCH_STATUS_OK;
}

static honch_status_t honch_esp_ram_queue_peek(
    honch_esp_storage_t *storage,
    honch_storage_reader_t *reader)
{
    if (storage == NULL || reader == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (!honch_esp_ram_queue_has_events(storage)) {
        return HONCH_STATUS_ERROR_NOT_INITIALIZED;
    }

    uint64_t after = storage->peek_sequence;
    for (size_t i = 0u; i < storage->ram_count; i++) {
        honch_esp_ram_queue_entry_t *entry = &storage->ram_entries[i];
        if (after != UINT64_MAX && entry->sequence <= after) {
            continue;
        }

        storage->peek_sequence = entry->sequence;
        storage->read_sequence = entry->sequence;
        *reader = (honch_storage_reader_t) {
            .ctx = storage,
            .read = honch_esp_ram_reader_read,
            .total_size = entry->size,
            .sequence = entry->sequence
        };
        return HONCH_STATUS_OK;
    }

    return HONCH_STATUS_ERROR_NOT_INITIALIZED;
}

static void honch_esp_ram_queue_remove_at(honch_esp_storage_t *storage, size_t index)
{
    honch_esp_ram_queue_entry_t removed = storage->ram_entries[index];
    size_t removed_end = (size_t)removed.offset + removed.size;
    size_t remaining_bytes = storage->ram_used - removed_end;

    if (remaining_bytes > 0u) {
        memmove(
            storage->ram_buffer + removed.offset,
            storage->ram_buffer + removed_end,
            remaining_bytes);
    }

    for (size_t i = 0u; i < storage->ram_count; i++) {
        if (storage->ram_entries[i].offset > removed.offset) {
            storage->ram_entries[i].offset -= removed.size;
        }
    }
    for (size_t i = index + 1u; i < storage->ram_count; i++) {
        storage->ram_entries[i - 1u] = storage->ram_entries[i];
    }

    storage->ram_count--;
    storage->ram_used -= removed.size;
    storage->peek_sequence = UINT64_MAX;
    storage->read_sequence = UINT64_MAX;
}

static honch_status_t honch_esp_ram_queue_consume(honch_esp_storage_t *storage, uint64_t sequence)
{
    if (storage == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    for (size_t i = 0u; i < storage->ram_count; i++) {
        if (storage->ram_entries[i].sequence == sequence) {
            honch_esp_ram_queue_remove_at(storage, i);
            return HONCH_STATUS_OK;
        }
    }

    return HONCH_STATUS_ERROR_NOT_INITIALIZED;
}

static honch_status_t honch_esp_queue_drop_oldest(void *ctx)
{
    (void)ctx;
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_STATUS_OK || tail >= head) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(tail, key);
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        tail++;
        status = honch_esp_advance_tail_locked(handle, head, &tail);
    } else {
        status = honch_esp_nvs_status(err);
    }

    nvs_close(handle);
    return status;
}

static honch_status_t honch_esp_nvs_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    if (event == NULL || event_size == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    while (status == HONCH_STATUS_OK && head >= tail && head - tail >= HONCH_ESP_DEFAULT_MAX_QUEUE_DEPTH) {
        status = honch_esp_queue_drop_oldest(ctx);
        if (status == HONCH_STATUS_OK) {
            status = honch_esp_get_head_tail(&head, &tail);
        }
    }
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    uint64_t stored_sequence = sequence < head ? head : sequence;
    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(stored_sequence, key);
    err = nvs_set_blob(handle, key, event, event_size);
    if (err == ESP_OK) {
        head = stored_sequence + 1u;
    }
    if (err == ESP_OK) {
        status = honch_esp_advance_tail_locked(handle, head, &tail);
    } else {
        status = honch_esp_nvs_status(err);
    }

    nvs_close(handle);
    return status;
}

static honch_status_t honch_esp_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (storage == NULL || buffer == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(storage->read_sequence, key);
    size_t value_size = 0u;
    err = nvs_get_blob(handle, key, NULL, &value_size);
    if (err == ESP_OK && (offset > value_size || buffer_size > value_size - offset)) {
        err = ESP_ERR_INVALID_ARG;
    }

    if (err == ESP_OK) {
        uint8_t *scratch = (uint8_t *)malloc(value_size);
        if (scratch == NULL) {
            nvs_close(handle);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        size_t read_size = value_size;
        err = nvs_get_blob(handle, key, scratch, &read_size);
        if (err == ESP_OK) {
            memcpy(buffer, scratch + offset, buffer_size);
        }
        free(scratch);
    }

    nvs_close(handle);
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_nvs_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (storage == NULL || reader == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_STATUS_ERROR_NOT_INITIALIZED;
    }
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    uint64_t start = storage->peek_sequence == UINT64_MAX ? tail : storage->peek_sequence + 1u;
    for (uint64_t sequence = start; sequence < head; sequence++) {
        size_t value_size = 0u;
        if (!honch_esp_queue_blob_exists(handle, sequence, &value_size)) {
            continue;
        }

        storage->peek_sequence = sequence;
        storage->read_sequence = sequence;
        *reader = (honch_storage_reader_t) {
            .ctx = storage,
            .read = honch_esp_reader_read,
            .total_size = value_size,
            .sequence = sequence
        };
        nvs_close(handle);
        return HONCH_STATUS_OK;
    }

    nvs_close(handle);
    return HONCH_STATUS_ERROR_NOT_INITIALIZED;
}

static honch_status_t honch_esp_queue_erase_sequence(uint64_t sequence)
{
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(sequence, key);
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        if (sequence == tail) {
            tail++;
        }
        status = honch_esp_advance_tail_locked(handle, head, &tail);
    } else {
        status = honch_esp_nvs_status(err);
    }

    nvs_close(handle);
    return status;
}

static honch_status_t honch_esp_nvs_queue_consume(void *ctx, uint64_t sequence)
{
    (void)ctx;
    return honch_esp_queue_erase_sequence(sequence);
}

static honch_status_t honch_esp_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    honch_status_t status = honch_esp_ram_queue_push(storage, event, event_size, sequence);
    if (status == HONCH_STATUS_OK) {
        return HONCH_STATUS_OK;
    }
    if (status != HONCH_STATUS_ERROR_QUEUE_FULL) {
        return status;
    }

    if (storage != NULL) {
        storage->nvs_fallback_active = true;
    }
    return honch_esp_nvs_queue_push(ctx, event, event_size, sequence);
}

static honch_status_t honch_esp_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (honch_esp_ram_queue_has_events(storage)) {
        return honch_esp_ram_queue_peek(storage, reader);
    }

    return honch_esp_nvs_queue_peek(ctx, reader);
}

static honch_status_t honch_esp_queue_consume(void *ctx, uint64_t sequence)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    honch_status_t status = honch_esp_ram_queue_consume(storage, sequence);
    if (status == HONCH_STATUS_OK) {
        return HONCH_STATUS_OK;
    }
    if (status != HONCH_STATUS_ERROR_NOT_INITIALIZED) {
        return status;
    }

    status = honch_esp_nvs_queue_consume(ctx, sequence);
    if (status == HONCH_STATUS_OK && storage != NULL) {
        size_t depth = 0u;
        if (honch_esp_queue_depth(ctx, &depth) == HONCH_STATUS_OK && depth == 0u) {
            storage->nvs_fallback_active = false;
        }
    }
    return status;
}

static honch_status_t honch_esp_queue_dead_letter(void *ctx, uint64_t sequence)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    honch_esp_ram_queue_entry_t *ram_entry = honch_esp_ram_queue_find(storage, sequence);
    if (ram_entry != NULL) {
        char key[HONCH_ESP_NVS_KEY_SIZE];
        honch_esp_sequence_key(sequence, key);
        nvs_handle_t dead_handle = 0;
        esp_err_t err = nvs_open(HONCH_ESP_DEAD_NAMESPACE, NVS_READWRITE, &dead_handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(
                dead_handle,
                key,
                storage->ram_buffer + ram_entry->offset,
                ram_entry->size);
        }
        if (err == ESP_OK) {
            err = nvs_commit(dead_handle);
        }
        if (dead_handle != 0) {
            nvs_close(dead_handle);
        }
        if (err != ESP_OK) {
            return honch_esp_nvs_status(err);
        }
        return honch_esp_ram_queue_consume(storage, sequence);
    }

    nvs_handle_t queue_handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &queue_handle);
    if (err != ESP_OK) {
        return honch_esp_nvs_status(err);
    }

    char key[HONCH_ESP_NVS_KEY_SIZE];
    honch_esp_sequence_key(sequence, key);
    size_t value_size = 0u;
    err = nvs_get_blob(queue_handle, key, NULL, &value_size);
    if (err == ESP_OK && value_size > 0u) {
        uint8_t *scratch = (uint8_t *)malloc(value_size);
        if (scratch == NULL) {
            nvs_close(queue_handle);
            return HONCH_STATUS_ERROR_OUT_OF_MEMORY;
        }
        size_t read_size = value_size;
        err = nvs_get_blob(queue_handle, key, scratch, &read_size);
        if (err == ESP_OK) {
            nvs_handle_t dead_handle = 0;
            esp_err_t dead_err = nvs_open(HONCH_ESP_DEAD_NAMESPACE, NVS_READWRITE, &dead_handle);
            if (dead_err == ESP_OK) {
                dead_err = nvs_set_blob(dead_handle, key, scratch, read_size);
            }
            if (dead_err == ESP_OK) {
                dead_err = nvs_commit(dead_handle);
            }
            if (dead_err == ESP_OK || dead_handle != 0) {
                nvs_close(dead_handle);
            }
            if (dead_err != ESP_OK) {
                err = dead_err;
            }
        }
        free(scratch);
    }
    nvs_close(queue_handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return honch_esp_nvs_status(err);
    }

    return honch_esp_queue_erase_sequence(sequence);
}

static honch_status_t honch_esp_queue_clear(void *ctx)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (storage != NULL) {
            honch_esp_ram_queue_reset(storage);
            storage->nvs_fallback_active = false;
        }
        return HONCH_STATUS_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK || handle != 0) {
        nvs_close(handle);
    }
    if (storage != NULL) {
        honch_esp_ram_queue_reset(storage);
        storage->nvs_fallback_active = false;
    }
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_queue_depth(void *ctx, size_t *depth)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (depth == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (storage != NULL) {
        storage->peek_sequence = UINT64_MAX;
    }

    size_t nvs_depth = 0u;
    honch_status_t status = honch_esp_nvs_queue_depth(&nvs_depth);
    if (status != HONCH_STATUS_OK) {
        return status;
    }

    *depth = nvs_depth;
    if (storage != NULL) {
        *depth += storage->ram_count;
        if (nvs_depth == 0u) {
            storage->nvs_fallback_active = false;
        }
    }
    return HONCH_STATUS_OK;
}

honch_status_t honch_esp_storage_ops_init(honch_storage_ops_t *ops, honch_esp_storage_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (honch_esp_storage_t) {
        .peek_sequence = UINT64_MAX,
        .read_sequence = UINT64_MAX
    };
    *ops = (honch_storage_ops_t) {
        .state_get = honch_esp_state_get,
        .state_set = honch_esp_state_set,
        .state_delete = honch_esp_state_delete,
        .queue_push = honch_esp_queue_push,
        .queue_peek = honch_esp_queue_peek,
        .queue_consume = honch_esp_queue_consume,
        .queue_dead_letter = honch_esp_queue_dead_letter,
        .queue_drop_oldest = honch_esp_queue_drop_oldest,
        .queue_clear = honch_esp_queue_clear,
        .queue_depth = honch_esp_queue_depth,
        .ctx = ctx
    };
    return HONCH_STATUS_OK;
}

honch_status_t honch_esp_storage_use_ram_queue(
    honch_esp_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size)
{
    if (ctx == NULL || buffer == NULL || buffer_size == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    uintptr_t buffer_start = (uintptr_t)buffer;
    uintptr_t entries_start = honch_esp_align_up(buffer_start, _Alignof(honch_esp_ram_queue_entry_t));
    size_t alignment_padding = (size_t)(entries_start - buffer_start);
    if (alignment_padding >= buffer_size) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    size_t usable_size = buffer_size - alignment_padding;
    size_t entry_capacity = usable_size / (sizeof(honch_esp_ram_queue_entry_t) + 1u);
    if (entry_capacity > HONCH_ESP_RAM_QUEUE_MAX_EVENTS) {
        entry_capacity = HONCH_ESP_RAM_QUEUE_MAX_EVENTS;
    }
    if (entry_capacity == 0u) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    size_t entry_bytes = entry_capacity * sizeof(honch_esp_ram_queue_entry_t);
    if (entry_bytes >= usable_size) {
        return HONCH_STATUS_ERROR_INVALID_ARGUMENT;
    }

    ctx->ram_entries = (honch_esp_ram_queue_entry_t *)entries_start;
    ctx->ram_entry_capacity = entry_capacity;
    ctx->ram_buffer = (uint8_t *)(entries_start + entry_bytes);
    ctx->ram_buffer_size = usable_size - entry_bytes;
    honch_esp_ram_queue_reset(ctx);

    size_t nvs_depth = 0u;
    honch_status_t status = honch_esp_nvs_queue_depth(&nvs_depth);
    if (status != HONCH_STATUS_OK) {
        return status;
    }
    ctx->nvs_fallback_active = nvs_depth > 0u;
    return HONCH_STATUS_OK;
}
