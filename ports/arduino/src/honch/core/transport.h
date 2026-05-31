#ifndef HONCH_CORE_TRANSPORT_H
#define HONCH_CORE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

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
} honch_transport_ops_t;

#ifdef __cplusplus
}
#endif

#endif
