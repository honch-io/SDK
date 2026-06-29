#ifndef HONCH_CORE_ERROR_DETAIL_H
#define HONCH_CORE_ERROR_DETAIL_H

#include "honch/core/status.h"
#include "honch/core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Finer-grained cause behind a honch_status_t. ADD values; never renumber or
 * remove one — shipped firmware and integrators may compare against the
 * numeric value. HONCH_REASON_NONE (0) is the "no error / not set" state, so a
 * zero-initialised honch_error_detail_t reads as "no error". */
typedef enum honch_error_reason {
    HONCH_REASON_NONE = 0,
    HONCH_REASON_UNKNOWN,
    /* transport / network */
    HONCH_REASON_DNS_FAILED,
    HONCH_REASON_CONNECT_REFUSED,
    HONCH_REASON_CONNECT_TIMEOUT,
    HONCH_REASON_TLS_HANDSHAKE,
    HONCH_REASON_TLS_CERT,
    HONCH_REASON_WRITE_FAILED,
    HONCH_REASON_READ_FAILED,
    HONCH_REASON_HTTP_STATUS,      /* non-2xx HTTP; numeric code in http_status */
    HONCH_REASON_AUTH_INVALID_KEY, /* HTTP 401 */
    HONCH_REASON_OFFLINE,
    /* local / pipeline */
    HONCH_REASON_QUEUE_FULL,
    HONCH_REASON_ENCODE_FAILED,
    HONCH_REASON_OUT_OF_MEMORY,
    HONCH_REASON_INVALID_CONFIG,
    HONCH_REASON_NOT_INITIALIZED
} honch_error_reason_t;

/* Structured detail for the most recent failure. Caller-owned (copied out by
 * honch_core_get_last_error); contains no owned allocations. `message` and
 * `component` point to static string literals valid for the process lifetime,
 * so the struct may be copied and outlive the call freely. */
typedef struct honch_error_detail {
    honch_status_t           status;           /* coarse code (unchanged contract) */
    honch_error_reason_t     reason;           /* finer cause */
    honch_transport_result_t transport_result; /* transport classification, if any */
    int                      http_status;      /* HTTP status code, 0 if not applicable */
    int                      os_error;         /* errno / esp_err_t etc., 0 if not applicable */
    const char              *message;          /* short static description, or NULL */
    const char              *component;        /* failure phase, e.g. "http"/"queue", or NULL */
} honch_error_detail_t;

#ifdef __cplusplus
}
#endif

#endif /* HONCH_CORE_ERROR_DETAIL_H */
