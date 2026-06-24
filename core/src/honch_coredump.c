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

#if HONCH_ENABLE_CRASH_CAPTURE
/*
 * Post at most one coredump frame, streaming the image from flash. Cooperative:
 * called repeatedly from the flush/tick path with the client lock held and
 * connectivity already confirmed (network I/O under the lock matches the event
 * flush). The committed CRC and offset advance ONLY on a successful ack, so a
 * retried chunk is never double-counted in the running CRC. On the final frame's
 * ack the source is flagged for clearing (erase-after-ack); the caller performs
 * the actual clear() after releasing the lock, since a flash erase must not run
 * under the state lock. `*progressed` is set when a frame was accepted.
 */
honch_status_t honch_coredump_upload_step_locked(honch_client_t *client, bool *progressed)
{
    if (progressed != NULL) {
        *progressed = false;
    }
    const honch_coredump_source_t *source = client->coredump_source;
    if (source == NULL || source->size == NULL || source->read == NULL ||
        client->transport == NULL || client->transport->post_chunk == NULL) {
        return HONCH_OK;
    }

    if (!client->coredump_upload_active) {
        /* Only upload once a $crash has been reported: its crash_id is the
         * stream_id that links this blob to the summary. No crash, no upload. */
        if (client->coredump_crash_id[0] == '\0') {
            return HONCH_OK;
        }
        size_t total = source->size(source->ctx);
        if (total == 0u) {
            return HONCH_OK; /* nothing on the device */
        }
        client->coredump_total = total;
        client->coredump_offset = 0u;
        client->coredump_committed_crc = HONCH_WIRE_V2_CRC16_INITIAL;
        client->coredump_message_id = client->wire_v2_message_id_seed;
        client->coredump_upload_active = true;
    }

    size_t got = 0u;
    honch_status_t status = honch_coredump_chunk(
        source, client->coredump_offset, client->coredump_chunk, HONCH_COREDUMP_CHUNK_BYTES, &got);
    if (status != HONCH_OK || got == 0u) {
        /* Read error (or an unexpectedly empty read): abandon this attempt without
         * clearing the source, so it survives for a later retry/boot. */
        client->coredump_upload_active = false;
        return status;
    }

    bool more = (client->coredump_offset + got) < client->coredump_total;
    /* Tentative CRC including this chunk; committed only if the post is acked. */
    uint16_t frame_crc = honch_wire_v2_crc16_update(
        client->coredump_committed_crc, client->coredump_chunk, got);

    honch_wire_v2_frame_spec_t spec = {
        .message_id = client->coredump_message_id,
        .total_message_length = client->coredump_total,
        .offset = client->coredump_offset,
        .payload = client->coredump_chunk,
        .payload_size = got,
        .source_type = HONCH_WIRE_V2_SOURCE_COREDUMP,
        .continuation = client->coredump_offset > 0u,
        .more = more
    };
    if (!more) {
        spec.has_precomputed_crc = true;
        spec.precomputed_crc = frame_crc;
    }

    size_t frame_size = 0u;
    status = honch_wire_v2_encode_frame(
        &spec, client->coredump_frame, sizeof(client->coredump_frame), &frame_size);
    if (status != HONCH_OK) {
        client->coredump_upload_active = false;
        return status;
    }

    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    status = client->transport->post_chunk(
        client->transport->ctx,
        client->endpoint_url,
        client->api_key,
        client->coredump_crash_id, /* stream_id: links the blob to its $crash */
        client->coredump_frame,
        frame_size,
        &result);
    client->outbound_upload_attempted = true;

    if (status != HONCH_OK) {
        /* Permanent rejection abandons the upload (the next boot decides what to
         * do with the still-present source); retryable failures keep all state so
         * the same offset is re-sent next time. Either way, no CRC/offset advance. */
        if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
            client->coredump_upload_active = false;
        }
        return status;
    }

    /* Accepted (ACCEPTED or CHUNK_STORED): commit progress exactly once. */
    client->coredump_committed_crc = frame_crc;
    client->coredump_offset += got;
    if (progressed != NULL) {
        *progressed = true;
    }
    if (!more) {
        client->coredump_upload_active = false;
        client->coredump_clear_due = true; /* erase-after-ack, fired outside the lock */
    }
    return HONCH_OK;
}
#endif /* HONCH_ENABLE_CRASH_CAPTURE */
