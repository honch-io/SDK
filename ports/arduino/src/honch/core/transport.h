#ifndef HONCH_CORE_TRANSPORT_H
#define HONCH_CORE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"
#include "honch/core/error_reason.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_transport_result {
    HONCH_TRANSPORT_ACCEPTED,
    HONCH_TRANSPORT_CHUNK_STORED,
    HONCH_TRANSPORT_RETRY,
    HONCH_TRANSPORT_REJECTED,
    HONCH_TRANSPORT_AUTH_ERROR
} honch_transport_result_t;

/* Optional richer outcome a transport may report alongside the coarse
 * honch_transport_result_t. All fields are "unknown" when zero-initialised, so
 * a transport need only set what it actually knows. */
typedef struct honch_transport_detail {
    int                  http_status; /* HTTP status code, 0 if none was received */
    int                  os_error;    /* errno / CURLcode / esp_err_t, 0 if none */
    honch_error_reason_t reason;      /* HONCH_REASON_NONE if the transport can't classify */
} honch_transport_detail_t;

typedef struct honch_transport_ops {
    honch_status_t (*post_chunk)(
        void *ctx,
        const char *endpoint_url,
        const char *api_key,
        const char *stream_id,
        const uint8_t *body,
        size_t body_size,
        honch_transport_result_t *result);
    uint64_t (*retry_after_ms)(void *ctx);
    void *ctx;
    /* Optional detailed variant (added; trailing for ABI/source compatibility).
     * When non-NULL the core prefers it and passes a honch_transport_detail_t
     * to capture http_status / os_error / reason. Ports that leave it NULL keep
     * working unchanged via post_chunk, with a coarse reason derived from the
     * transport result. */
    honch_status_t (*post_chunk_ex)(
        void *ctx,
        const char *endpoint_url,
        const char *api_key,
        const char *stream_id,
        const uint8_t *body,
        size_t body_size,
        honch_transport_result_t *result,
        honch_transport_detail_t *detail);
} honch_transport_ops_t;

#ifdef __cplusplus
}
#endif

#endif
