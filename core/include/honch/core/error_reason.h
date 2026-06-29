#ifndef HONCH_CORE_ERROR_REASON_H
#define HONCH_CORE_ERROR_REASON_H

#ifdef __cplusplus
extern "C" {
#endif

/* Finer-grained cause behind a honch_status_t / transport result. ADD values;
 * never renumber or remove one — shipped firmware and integrators may compare
 * against the numeric value. HONCH_REASON_NONE (0) is the "no error / not set"
 * state, so a zero-initialised struct reads as "no error".
 *
 * Lives in its own dependency-free header so transport.h (the detail struct)
 * and error_detail.h can both use it without a circular include. */
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

#ifdef __cplusplus
}
#endif

#endif /* HONCH_CORE_ERROR_REASON_H */
