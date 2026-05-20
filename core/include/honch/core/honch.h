#ifndef HONCH_CORE_HONCH_H
#define HONCH_CORE_HONCH_H

#include "honch/core/config.h"
#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_client honch_client_t;

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config);
honch_status_t honch_core_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_core_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_core_set_property(honch_client_t *client, const char *key, const char *value_json);
honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_core_session_end(honch_client_t *client);
honch_status_t honch_core_flush(honch_client_t *client);
honch_status_t honch_core_reset(honch_client_t *client);
honch_status_t honch_core_shutdown(honch_client_t *client);
const char *honch_core_get_device_id(honch_client_t *client);
honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
const char *honch_status_string(honch_status_t status);

#ifdef __cplusplus
}
#endif

#endif
