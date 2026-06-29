/* Self-contained error-detail helpers: reason stringification and one-line
 * formatting. Deliberately free of platform/state dependencies so a caller
 * (or a test) can link just this TU without dragging in the full client. */
#include <stdio.h>
#include <string.h>

#include "honch/core/honch.h"

const char *honch_status_string(honch_status_t status)
{
    switch (status) {
        case HONCH_OK:
            return "ok";
        case HONCH_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case HONCH_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case HONCH_ERROR_IO:
            return "io error";
        case HONCH_ERROR_TRANSPORT:
            return "transport error";
        case HONCH_ERROR_RATE_LIMITED:
            return "rate limited";
        case HONCH_ERROR_SERVER:
            return "server error";
        case HONCH_ERROR_REJECTED:
            return "rejected";
        case HONCH_ERROR_NOT_INITIALIZED:
            return "not initialized";
        case HONCH_ERROR_ALREADY_INITIALIZED:
            return "already initialized";
        case HONCH_ERROR_QUEUE_FULL:
            return "queue full";
        case HONCH_ERROR_TIMEOUT:
            return "timeout";
        case HONCH_ERROR_INTERNAL:
            return "internal error";
        case HONCH_ERROR_BUSY:
            return "busy";
        case HONCH_ERROR_NOT_SUPPORTED:
            return "not supported";
        case HONCH_ERROR_OFFLINE:
            return "offline";
        default:
            return "unknown";
    }
}

const char *honch_error_reason_string(honch_error_reason_t reason)
{
    switch (reason) {
        case HONCH_REASON_NONE:
            return "none";
        case HONCH_REASON_UNKNOWN:
            return "unknown";
        case HONCH_REASON_DNS_FAILED:
            return "dns_failed";
        case HONCH_REASON_CONNECT_REFUSED:
            return "connect_refused";
        case HONCH_REASON_CONNECT_TIMEOUT:
            return "connect_timeout";
        case HONCH_REASON_TLS_HANDSHAKE:
            return "tls_handshake";
        case HONCH_REASON_TLS_CERT:
            return "tls_cert";
        case HONCH_REASON_WRITE_FAILED:
            return "write_failed";
        case HONCH_REASON_READ_FAILED:
            return "read_failed";
        case HONCH_REASON_HTTP_STATUS:
            return "http_status";
        case HONCH_REASON_AUTH_INVALID_KEY:
            return "auth_invalid_key";
        case HONCH_REASON_OFFLINE:
            return "offline";
        case HONCH_REASON_QUEUE_FULL:
            return "queue_full";
        case HONCH_REASON_ENCODE_FAILED:
            return "encode_failed";
        case HONCH_REASON_OUT_OF_MEMORY:
            return "out_of_memory";
        case HONCH_REASON_INVALID_CONFIG:
            return "invalid_config";
        case HONCH_REASON_NOT_INITIALIZED:
            return "not_initialized";
        default:
            return "unknown";
    }
}

size_t honch_error_detail_format(const honch_error_detail_t *detail, char *buf, size_t buf_size)
{
    if (detail == NULL || buf == NULL || buf_size == 0u) {
        return 0u;
    }

    int written;
    if (detail->http_status > 0 && detail->message != NULL) {
        written = snprintf(
            buf, buf_size, "%s: HTTP %d - %s (reason=%s)",
            honch_status_string(detail->status), detail->http_status,
            detail->message, honch_error_reason_string(detail->reason));
    } else if (detail->http_status > 0) {
        written = snprintf(
            buf, buf_size, "%s: HTTP %d (reason=%s)",
            honch_status_string(detail->status), detail->http_status,
            honch_error_reason_string(detail->reason));
    } else if (detail->message != NULL) {
        written = snprintf(
            buf, buf_size, "%s: %s (reason=%s)",
            honch_status_string(detail->status), detail->message,
            honch_error_reason_string(detail->reason));
    } else {
        written = snprintf(
            buf, buf_size, "%s (reason=%s)",
            honch_status_string(detail->status),
            honch_error_reason_string(detail->reason));
    }

    if (written < 0) {
        buf[0] = '\0';
        return 0u;
    }
    /* snprintf returns the length it *would* have written; clamp to what fit. */
    size_t length = (size_t)written >= buf_size ? buf_size - 1u : (size_t)written;

    /* Append the OS/transport error code when present. This is the raw
     * errno / CURLcode / esp_err_t the transport surfaced — the single most
     * actionable value for a transport-phase failure (DNS/connect/TLS), and
     * the part the proactive auto-log line would otherwise drop. Built in a
     * local buffer and appended only when the base line fully fit AND the whole
     * suffix fits, so a truncated line is never corrupted and no partial token
     * is ever emitted. */
    if (detail->os_error != 0 && length == (size_t)written) {
        char suffix[24]; /* " os_error=" + INT_MIN (-2147483648) + NUL = 22 */
        int extra = snprintf(suffix, sizeof(suffix), " os_error=%d", detail->os_error);
        if (extra > 0 && (size_t)extra < buf_size - length) {
            memcpy(buf + length, suffix, (size_t)extra + 1u); /* includes the NUL */
            length += (size_t)extra;
        }
    }
    return length;
}
