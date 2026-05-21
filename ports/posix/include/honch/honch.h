#ifndef HONCH_HONCH_H
#define HONCH_HONCH_H

#include "honch/core/honch.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef honch_status_t honch_err_t;

#define HONCH_ERR_INVALID_ARG HONCH_ERROR_INVALID_ARGUMENT
#define HONCH_ERR_NOT_INITIALIZED HONCH_ERROR_NOT_INITIALIZED
#define HONCH_ERR_ALREADY_INITIALIZED HONCH_ERROR_ALREADY_INITIALIZED
#define HONCH_ERR_NO_MEM HONCH_ERROR_OUT_OF_MEMORY
#define HONCH_ERR_QUEUE_FULL HONCH_ERROR_QUEUE_FULL
#define HONCH_ERR_NVS HONCH_ERROR_IO
#define HONCH_ERR_TRANSPORT HONCH_ERROR_TRANSPORT
#define HONCH_ERR_TIMEOUT HONCH_ERROR_TIMEOUT
#define HONCH_ERR_INTERNAL HONCH_ERROR_INTERNAL

typedef struct honch_config {
    const char *api_key;
    const char *endpoint_url;
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *environment;
    const char *queue_directory;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    size_t flush_event_threshold;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    int disable_background_flush;
    int (*battery_callback)(void);
    int battery_low_threshold;
    honch_auto_properties_fn auto_properties_callback;
    void *auto_properties_userdata;
    honch_durability_mode_t durability_mode;
} honch_config_t;

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config);
honch_status_t honch_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_set_property(honch_client_t *client, const char *key, const char *value_json);
honch_status_t honch_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_session_end(honch_client_t *client);
honch_status_t honch_flush(honch_client_t *client);
honch_status_t honch_reset(honch_client_t *client);
honch_status_t honch_shutdown(honch_client_t *client);

const char *honch_get_device_id(honch_client_t *client);
honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
const char *honch_status_string(honch_status_t status);

#ifdef HONCH_TESTING
typedef honch_status_t (*honch_test_transport_fn)(
    const char *url,
    const char *api_key,
    const char *stream_id,
    const unsigned char *body,
    size_t body_size,
    const char *content_encoding,
    void *userdata,
    long *http_status);

void honch_test_set_transport(honch_test_transport_fn transport, void *userdata);
#endif

#ifdef __cplusplus
}
#endif

#endif
