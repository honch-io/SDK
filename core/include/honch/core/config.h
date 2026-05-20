#ifndef HONCH_CORE_CONFIG_H
#define HONCH_CORE_CONFIG_H

#include <stddef.h>

#include "honch/core/platform.h"
#include "honch/core/status.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct honch_core_config {
    const char *api_key;
    const char *endpoint_url;
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *environment;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    size_t flush_event_threshold;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    int disable_gzip;
    size_t gzip_min_bytes;
    int disable_background_flush;
    int (*battery_callback)(void);
    int battery_low_threshold;
    const honch_platform_ops_t *platform;
    const honch_storage_ops_t *storage;
    const honch_transport_ops_t *transport;
} honch_core_config_t;

#ifdef __cplusplus
}
#endif

#endif
