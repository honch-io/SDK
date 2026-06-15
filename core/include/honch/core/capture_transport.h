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

typedef struct honch_capture_stream_ops {
    honch_status_t (*open)(
        void *ctx,
        const honch_capture_endpoint_t *endpoint,
        unsigned int timeout_ms,
        void **stream);
    honch_status_t (*write)(
        void *ctx,
        void *stream,
        const uint8_t *data,
        size_t data_size,
        size_t *written);
    int (*read_byte)(
        void *ctx,
        void *stream,
        unsigned int timeout_ms);
    honch_status_t (*flush)(void *ctx, void *stream);
    void (*close)(void *ctx, void *stream, int success);
    const char *(*connection_type)(void *ctx);
    void *ctx;
} honch_capture_stream_ops_t;

typedef struct honch_capture_transport_config {
    const honch_capture_stream_ops_t *stream_ops;
    unsigned int timeout_ms;
    size_t max_write_bytes;
} honch_capture_transport_config_t;

typedef struct honch_capture_transport {
    honch_capture_stream_ops_t stream_ops;
    unsigned int timeout_ms;
    size_t max_write_bytes;
} honch_capture_transport_t;

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
honch_status_t honch_capture_transport_init(
    honch_transport_ops_t *transport_ops,
    honch_capture_transport_t *transport,
    const honch_capture_transport_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
