#include "honch_internal.h"

/*
 * Read one bounded chunk of the coredump image into `buffer`. Reads at most
 * `buffer_size` bytes (less near the end of the image), so the multi-KB image is
 * never materialized in RAM — the caller walks `offset` from 0 to the image size
 * one chunk at a time. `*out_size` is set to the bytes produced (0 when `offset`
 * is at the end). The image is read straight from the port's backing store.
 *
 * `total` is the image size SNAPSHOTTED once by the caller at the start of the
 * upload — this function never re-reads source->size(). Freezing the size keeps
 * (message_id, total_message_length) invariant for the life of one stream even
 * if the underlying store changes; the upload aborts cleanly if the snapshot
 * ever disagrees with reality (a short read surfaces as HONCH_ERROR_IO).
 */
honch_status_t honch_coredump_chunk(
    const honch_coredump_source_t *source,
    size_t total,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size)
{
    if (source == NULL || source->read == NULL ||
        buffer == NULL || out_size == NULL || buffer_size == 0u) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *out_size = 0u;
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
/* Abandon the in-flight upload: stop streaming and latch completion so it is
 * never restarted this session, and flag the source for erase. Used on a
 * permanent rejection — the summary already delivered, so re-uploading a
 * rejected blob every boot would only spam duplicate $crash events. */
static void honch_coredump_abandon(honch_client_t *client)
{
    client->coredump_upload_active = false;
    client->coredump_done = true;
    client->coredump_clear_due = true;
}

/*
 * Post at most one coredump frame, streaming the image from flash. Cooperative:
 * called repeatedly from the flush/tick path with the state lock held and
 * connectivity already confirmed. The committed CRC and offset advance ONLY when
 * the post is acked with the result the frame expects, so a retried/declined
 * chunk is never double-counted in the running CRC. The state lock is RELEASED
 * around the network post (mirroring the event flush) so a slow transport never
 * stalls other callers and a transport that re-enters flush/tick cannot deadlock
 * on the non-recursive state mutex. On the final frame's ack completion is
 * latched (`coredump_done`) and the source is flagged for clearing
 * (erase-after-ack); the caller performs the actual clear() after unlocking,
 * since a flash erase must not run under the state lock. `*progressed` is set
 * when a frame was accepted.
 */
honch_status_t honch_coredump_upload_step_locked(honch_client_t *client, bool *progressed)
{
    if (progressed != NULL) {
        *progressed = false;
    }
    /* Terminal latch: once an upload has fully delivered (or been abandoned), it
     * is NEVER restarted — checked before any source->size() so a NULL/async/
     * other-path clear() that leaves the image present can't trigger a re-stream. */
    if (client->coredump_done) {
        return HONCH_OK;
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
        client->coredump_total = total; /* snapshot once; never re-read per chunk */
        client->coredump_offset = 0u;
        client->coredump_committed_crc = HONCH_WIRE_V2_CRC16_INITIAL;
        client->coredump_message_id = client->wire_v2_message_id_seed;
        client->coredump_upload_active = true;
    }

    size_t got = 0u;
    honch_status_t status = honch_coredump_chunk(
        source, client->coredump_total, client->coredump_offset,
        client->coredump_chunk, HONCH_COREDUMP_CHUNK_BYTES, &got);
    if (status != HONCH_OK || got == 0u) {
        /* Transient read error or short read: keep the upload active and preserve
         * offset/CRC so the next drive RESUMES at the committed offset under the
         * same (message_id, total) — never a from-zero restart that would re-init
         * the same stream. The source is left intact for the retry. */
        return status != HONCH_OK ? status : HONCH_OK;
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
        /* Encode failure is a programming error, not a transport one: abandon so
         * we don't spin re-encoding the same frame forever. */
        honch_coredump_abandon(client);
        return status;
    }

    /* Release the state lock around the network post, exactly like the event
     * flush (honch_queue_policy.c): no blocking I/O under the lock, and a
     * re-entrant flush/tick from the transport callback hits the flush_in_progress
     * busy-guard instead of deadlocking on the non-recursive state mutex. Snapshot
     * the stream_id (crash_id) into a local under the lock first — every other
     * argument is immutable config and the frame/offset/CRC/`more` are captured in
     * locals above, so the post touches no shared state while the lock is dropped. */
    char stream_id[sizeof(client->coredump_crash_id)];
    memcpy(stream_id, client->coredump_crash_id, sizeof(stream_id));
    honch_transport_result_t result = HONCH_TRANSPORT_RETRY;
    honch_client_state_unlock(client);
    status = client->transport->post_chunk(
        client->transport->ctx,
        client->endpoint_url,
        client->api_key,
        stream_id, /* links the blob to its $crash */
        client->coredump_frame,
        frame_size,
        &result);
    honch_status_t relock_status = honch_client_state_lock(client);
    if (relock_status != HONCH_OK) {
        return relock_status;
    }
    client->outbound_upload_attempted = true;

    if (status != HONCH_OK) {
        /* Permanent rejection abandons the upload (and erases, so a rejected blob
         * is not retried — with the summary already delivered that would only
         * duplicate $crash events); retryable failures keep all state so the same
         * offset is re-sent next time. Either way, no CRC/offset advance. */
        if (result == HONCH_TRANSPORT_REJECTED || result == HONCH_TRANSPORT_AUTH_ERROR) {
            honch_coredump_abandon(client);
        }
        return status;
    }

    /* Commit on RESULT, not just status (mirrors the event path): a non-final
     * frame must be CHUNK_STORED and the final frame ACCEPTED. Any other pairing
     * — including (HONCH_OK, RETRY) from a non-conforming transport — retains the
     * chunk with no offset/CRC advance so it is re-sent, never folded into the
     * running CRC over bytes Capture did not store. */
    bool committed = more ? (result == HONCH_TRANSPORT_CHUNK_STORED)
                          : (result == HONCH_TRANSPORT_ACCEPTED);
    if (!committed) {
        return HONCH_OK; /* retain; resend this same offset next drive */
    }

    client->coredump_committed_crc = frame_crc;
    client->coredump_offset += got;
    if (progressed != NULL) {
        *progressed = true;
    }
    if (!more) {
        client->coredump_upload_active = false;
        client->coredump_done = true;      /* latch completion under the lock */
        client->coredump_clear_due = true; /* erase-after-ack, fired outside the lock */
    }
    return HONCH_OK;
}
#endif /* HONCH_ENABLE_CRASH_CAPTURE */
