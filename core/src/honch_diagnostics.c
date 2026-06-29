/* Failure diagnostics: capture the most recent failure as a honch_error_detail_t
 * on the client (always compiled — the accessor needs it) and, when
 * HONCH_ENABLE_ERROR_DIAGNOSTICS is on, emit one bounded, deduped log line per
 * distinct failure via platform->log.
 *
 * Re-entrancy: the emitted line is routed by the port under the SDK's own log
 * tag (HONCH_LOG_SELF_TAG), so the $error log-capture hook
 * (honch_core_report_log_error) skips it and can never turn the SDK's own
 * diagnostics into $error events — the failure class behind the 0.3.0 log-hook
 * self-deadlock. */
#include "honch_internal.h"

#include <string.h>

/* Human-readable static message for a reason (string literal, process-lifetime
 * — safe to store in honch_error_detail_t.message and copy out). */
static const char *honch_diag_reason_message(honch_error_reason_t reason)
{
    switch (reason) {
        case HONCH_REASON_AUTH_INVALID_KEY:
            return "API key invalid or revoked";
        case HONCH_REASON_DNS_FAILED:
            return "DNS resolution failed - check the configured endpoint";
        case HONCH_REASON_CONNECT_REFUSED:
            return "connection refused by the server";
        case HONCH_REASON_CONNECT_TIMEOUT:
            return "connection timed out";
        case HONCH_REASON_TLS_HANDSHAKE:
            return "TLS handshake failed";
        case HONCH_REASON_TLS_CERT:
            return "TLS certificate validation failed (check device clock / CA bundle)";
        case HONCH_REASON_WRITE_FAILED:
            return "network write failed";
        case HONCH_REASON_READ_FAILED:
            return "network read failed";
        case HONCH_REASON_HTTP_STATUS:
            return "server returned an error status";
        case HONCH_REASON_OFFLINE:
            return "device is offline";
        case HONCH_REASON_QUEUE_FULL:
            return "event queue full, dropping oldest events";
        case HONCH_REASON_ENCODE_FAILED:
            return "event encoding failed";
        case HONCH_REASON_OUT_OF_MEMORY:
            return "out of memory";
        case HONCH_REASON_INVALID_CONFIG:
            return "invalid endpoint or configuration";
        case HONCH_REASON_NOT_INITIALIZED:
            return "client not initialized";
        case HONCH_REASON_NONE:
        case HONCH_REASON_UNKNOWN:
        default:
            return NULL;
    }
}

/* When a transport reports only the coarse result (no post_chunk_ex), derive a
 * best-effort reason so the accessor still carries more than "transport error". */
static honch_error_reason_t honch_diag_reason_from_result(
    honch_transport_result_t result,
    honch_status_t status)
{
    if (result == HONCH_TRANSPORT_AUTH_ERROR) {
        return HONCH_REASON_AUTH_INVALID_KEY;
    }
    if (result == HONCH_TRANSPORT_REJECTED) {
        return HONCH_REASON_HTTP_STATUS;
    }
    if (status == HONCH_ERROR_OFFLINE) {
        return HONCH_REASON_OFFLINE;
    }
    return HONCH_REASON_UNKNOWN;
}

#if HONCH_ENABLE_ERROR_DIAGNOSTICS
static honch_log_level_t honch_diag_level(const honch_error_detail_t *detail)
{
    if (detail->transport_result == HONCH_TRANSPORT_REJECTED ||
        detail->transport_result == HONCH_TRANSPORT_AUTH_ERROR) {
        return HONCH_LOG_ERROR;
    }
    switch (detail->reason) {
        case HONCH_REASON_AUTH_INVALID_KEY:
        case HONCH_REASON_TLS_CERT:
        case HONCH_REASON_ENCODE_FAILED:
        case HONCH_REASON_OUT_OF_MEMORY:
        case HONCH_REASON_INVALID_CONFIG:
            return HONCH_LOG_ERROR;
        default:
            return HONCH_LOG_WARN;
    }
}

/* FNV-1a over (reason, http_status); 0 is reserved as "empty slot". */
static uint32_t honch_diag_hash(const honch_error_detail_t *detail)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)detail->reason) * 16777619u;
    h = (h ^ (uint32_t)detail->http_status) * 16777619u;
    return h == 0u ? 1u : h;
}

static bool honch_diag_already_logged(const honch_client_t *client, uint32_t hash)
{
    for (size_t i = 0u; i < client->diag_log_hash_count; i++) {
        if (client->diag_log_hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

static void honch_diag_remember(honch_client_t *client, uint32_t hash)
{
    if (client->diag_log_hash_count < HONCH_DIAG_DEDUP_SLOTS) {
        client->diag_log_hashes[client->diag_log_hash_count++] = hash;
        return;
    }
    /* Table full: drop the oldest, keep the most recent distinct failures. */
    for (size_t i = 1u; i < HONCH_DIAG_DEDUP_SLOTS; i++) {
        client->diag_log_hashes[i - 1u] = client->diag_log_hashes[i];
    }
    client->diag_log_hashes[HONCH_DIAG_DEDUP_SLOTS - 1u] = hash;
}

void honch_diag_log_failure_locked(honch_client_t *client)
{
    if (client == NULL || client->platform == NULL || client->platform->log == NULL) {
        return;
    }
    uint32_t hash = honch_diag_hash(&client->last_error);
    if (honch_diag_already_logged(client, hash)) {
        return; /* identical failure already surfaced; don't spam per retry */
    }
    honch_diag_remember(client, hash);

    char line[HONCH_DIAG_LINE_BYTES];
    (void)honch_error_detail_format(&client->last_error, line, sizeof(line));
    client->platform->log(
        client->platform->ctx, honch_diag_level(&client->last_error), line);
}

void honch_diag_note_success_locked(honch_client_t *client)
{
    if (client != NULL) {
        client->diag_log_hash_count = 0u;
    }
}
#else  /* diagnostics emission compiled out: capture still runs, logging does not */
void honch_diag_log_failure_locked(honch_client_t *client) { (void)client; }
void honch_diag_note_success_locked(honch_client_t *client) { (void)client; }
#endif

void honch_diag_capture_transport_locked(
    honch_client_t *client,
    honch_status_t status,
    honch_transport_result_t result,
    const honch_transport_detail_t *detail)
{
    if (client == NULL) {
        return;
    }
    honch_error_detail_t d = {0};
    d.status = status;
    d.transport_result = result;
    if (detail != NULL) {
        d.http_status = detail->http_status;
        d.os_error = detail->os_error;
        d.reason = detail->reason;
    }
    if (d.reason == HONCH_REASON_NONE) {
        d.reason = honch_diag_reason_from_result(result, status);
    }
    d.message = honch_diag_reason_message(d.reason);
    d.component = "http";
    client->last_error = d;
    honch_diag_log_failure_locked(client);
}

void honch_diag_capture_local_locked(
    honch_client_t *client,
    honch_status_t status,
    honch_error_reason_t reason,
    const char *component)
{
    if (client == NULL) {
        return;
    }
    honch_error_detail_t d = {0};
    d.status = status;
    d.reason = reason;
    d.message = honch_diag_reason_message(reason);
    d.component = component;
    client->last_error = d;
    honch_diag_log_failure_locked(client);
}
