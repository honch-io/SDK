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

static honch_status_t honch_esp_nvs_status(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
        case ESP_ERR_NVS_NOT_FOUND:
            return HONCH_OK;
        case ESP_ERR_NO_MEM:
            return HONCH_ERROR_OUT_OF_MEMORY;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_NVS_INVALID_LENGTH:
            return HONCH_ERROR_INVALID_ARGUMENT;
        default:
            return HONCH_ERROR_IO;
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
        return HONCH_OK;
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
    if (status == HONCH_OK) {
        status = honch_esp_get_counter("tail", tail);
    }
    if (status == HONCH_OK && *tail > *head) {
        *tail = *head;
    }
    return status;
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
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *buffer_size = 0u;
        return HONCH_OK;
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
        return HONCH_OK;
    }

    *buffer_size = value_size;
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_state_set(void *ctx, const char *key, const uint8_t *data, size_t data_size)
{
    (void)ctx;
    if (key == NULL || (data == NULL && data_size > 0u)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
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
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_OK;
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

static honch_status_t honch_esp_queue_drop_oldest(void *ctx)
{
    (void)ctx;
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_OK || tail >= head) {
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

static honch_status_t honch_esp_queue_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence)
{
    if (event == NULL || event_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    while (status == HONCH_OK && head >= tail && head - tail >= HONCH_ESP_DEFAULT_MAX_QUEUE_DEPTH) {
        status = honch_esp_queue_drop_oldest(ctx);
        if (status == HONCH_OK) {
            status = honch_esp_get_head_tail(&head, &tail);
        }
    }
    if (status != HONCH_OK) {
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
        return HONCH_ERROR_INVALID_ARGUMENT;
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
            return HONCH_ERROR_OUT_OF_MEMORY;
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

static honch_status_t honch_esp_queue_peek(void *ctx, honch_storage_reader_t *reader)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (storage == NULL || reader == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_OK) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_ERROR_NOT_INITIALIZED;
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
        return HONCH_OK;
    }

    nvs_close(handle);
    return HONCH_ERROR_NOT_INITIALIZED;
}

static honch_status_t honch_esp_queue_erase_sequence(uint64_t sequence)
{
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_OK) {
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

static honch_status_t honch_esp_queue_consume(void *ctx, uint64_t sequence)
{
    (void)ctx;
    return honch_esp_queue_erase_sequence(sequence);
}

static honch_status_t honch_esp_queue_dead_letter(void *ctx, uint64_t sequence)
{
    (void)ctx;
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
            return HONCH_ERROR_OUT_OF_MEMORY;
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
            storage->peek_sequence = UINT64_MAX;
            storage->read_sequence = UINT64_MAX;
        }
        return HONCH_OK;
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
        storage->peek_sequence = UINT64_MAX;
        storage->read_sequence = UINT64_MAX;
    }
    return honch_esp_nvs_status(err);
}

static honch_status_t honch_esp_queue_depth(void *ctx, size_t *depth)
{
    honch_esp_storage_t *storage = (honch_esp_storage_t *)ctx;
    if (depth == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (storage != NULL) {
        storage->peek_sequence = UINT64_MAX;
    }

    *depth = 0u;
    uint64_t head = 0u;
    uint64_t tail = 0u;
    honch_status_t status = honch_esp_get_head_tail(&head, &tail);
    if (status != HONCH_OK) {
        return status;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(HONCH_ESP_QUEUE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return HONCH_OK;
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
    return HONCH_OK;
}

honch_status_t honch_esp_storage_ops_init(honch_storage_ops_t *ops, honch_esp_storage_t *ctx)
{
    if (ops == NULL || ctx == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    ctx->peek_sequence = UINT64_MAX;
    ctx->read_sequence = UINT64_MAX;
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
    return HONCH_OK;
}
