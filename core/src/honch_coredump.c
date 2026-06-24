#include "honch_internal.h"

/*
 * Read one bounded chunk of the coredump image into `buffer`. Reads at most
 * `buffer_size` bytes (less near the end of the image), so the multi-KB image is
 * never materialized in RAM — the caller walks `offset` from 0 to the image size
 * one chunk at a time. `*out_size` is set to the bytes produced (0 when `offset`
 * is at the end). The image is read straight from the port's backing store.
 */
honch_status_t honch_coredump_chunk(
    const honch_coredump_source_t *source,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size)
{
    if (source == NULL || source->size == NULL || source->read == NULL ||
        buffer == NULL || out_size == NULL || buffer_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *out_size = 0u;
    size_t total = source->size(source->ctx);
    if (offset > total) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    size_t remaining = total - offset;
    if (remaining == 0u) {
        return HONCH_OK;
    }

    size_t length = remaining < buffer_size ? remaining : buffer_size;
    int read = source->read(source->ctx, offset, buffer, length);
    if (read < 0 || (size_t)read != length) {
        return HONCH_ERROR_IO;
    }

    *out_size = length;
    return HONCH_OK;
}
