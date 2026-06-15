#ifndef HONCH_CORE_CAPTURE_TRANSPORT_H
#define HONCH_CORE_CAPTURE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"
#include "honch/core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_capture_endpoint {
    int tls;
    char host[96];
    uint16_t port;
    char path[160];
} honch_capture_endpoint_t;

honch_status_t honch_capture_parse_endpoint(const char *endpoint_url, honch_capture_endpoint_t *out);
honch_status_t honch_capture_build_request_head(
    char *buffer,
    size_t buffer_size,
    size_t *written,
    const char *path,
    const char *host,
    size_t body_size,
    const char *api_key,
    const char *stream_id,
    const char *connection_type);
honch_status_t honch_capture_map_http_status(int status_code, honch_transport_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
