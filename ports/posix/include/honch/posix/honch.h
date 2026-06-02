#ifndef HONCH_POSIX_HONCH_H
#define HONCH_POSIX_HONCH_H

#include "honch/core/honch.h"
#include "honch/core/platform.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

typedef struct honch_posix_platform {
    void *reserved;
} honch_posix_platform_t;

typedef struct honch_posix_storage {
    honch_client_t *client;
    const char *queue_directory;
} honch_posix_storage_t;

typedef struct honch_posix_transport {
    honch_client_t *client;
    void *curl;
} honch_posix_transport_t;

honch_status_t honch_posix_platform_ops_init(honch_platform_ops_t *ops, honch_posix_platform_t *ctx);
honch_status_t honch_posix_storage_ops_init(
    honch_state_storage_ops_t *state_ops,
    honch_event_queue_ops_t *queue_ops,
    honch_posix_storage_t *ctx,
    const char *queue_directory);
honch_status_t honch_posix_transport_ops_init(honch_transport_ops_t *ops, honch_posix_transport_t *ctx);
void honch_posix_transport_ops_deinit(honch_posix_transport_t *ctx);
honch_status_t honch_posix_transport_post_chunk(
    honch_client_t *client,
    const char *stream_id,
    const unsigned char *payload,
    size_t payload_size,
    honch_transport_result_t *result);

#endif
