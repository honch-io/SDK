#ifndef HONCH_HONCH_H
#define HONCH_HONCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_client honch_client_t;

typedef enum honch_status {
    HONCH_OK = 0,
    HONCH_ERROR_INVALID_ARGUMENT = 1,
    HONCH_ERROR_OUT_OF_MEMORY = 2,
    HONCH_ERROR_IO = 3,
    HONCH_ERROR_TRANSPORT = 4,
    HONCH_ERROR_RATE_LIMITED = 5,
    HONCH_ERROR_SERVER = 6,
    HONCH_ERROR_REJECTED = 7
} honch_status_t;

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
} honch_config_t;

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config);
honch_status_t honch_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_set_property(honch_client_t *client, const char *key, const char *value_json);
honch_status_t honch_flush(honch_client_t *client);
honch_status_t honch_reset(honch_client_t *client);
void honch_shutdown(honch_client_t *client);

const char *honch_get_device_id(honch_client_t *client);
const char *honch_status_string(honch_status_t status);

#ifdef HONCH_TESTING
typedef honch_status_t (*honch_test_transport_fn)(
    const char *url,
    const char *api_key,
    const char *payload,
    void *userdata,
    long *http_status);

void honch_test_set_transport(honch_test_transport_fn transport, void *userdata);
#endif

#ifdef __cplusplus
}
#endif

#endif
