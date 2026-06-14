#ifndef HONCH_CORE_HONCH_H
#define HONCH_CORE_HONCH_H

#include "honch/core/config.h"
#include "honch/core/status.h"
#include "honch/core/wire_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_client honch_client_t;
typedef honch_wire_v2_value_t honch_value_t;
typedef honch_wire_v2_property_t honch_property_t;
typedef honch_wire_v2_map_pair_t honch_map_pair_t;

typedef enum honch_error_severity {
    HONCH_ERROR_SEVERITY_INFO = 0,
    HONCH_ERROR_SEVERITY_WARNING = 1,
    HONCH_ERROR_SEVERITY_ERROR = 2,
    HONCH_ERROR_SEVERITY_FATAL = 3
} honch_error_severity_t;

typedef struct honch_error_report {
    honch_error_severity_t severity;
    const char *message;
    const char *type;
    const char *component;
    const char *code;
    const char *backtrace;
} honch_error_report_t;

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config);
honch_status_t honch_core_track(
    honch_client_t *client,
    const char *event_name,
    const honch_property_t *properties,
    size_t property_count);
honch_status_t honch_core_identify(
    honch_client_t *client,
    const char *distinct_id,
    const honch_property_t *traits,
    size_t trait_count);
honch_status_t honch_core_report_error(
    honch_client_t *client,
    const honch_error_report_t *report,
    const honch_property_t *properties,
    size_t property_count);
honch_status_t honch_core_set_property(honch_client_t *client, const char *key, honch_value_t value);
honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_core_session_end(honch_client_t *client);
honch_status_t honch_core_tick(honch_client_t *client);
honch_status_t honch_core_flush(honch_client_t *client);
honch_status_t honch_core_reset(honch_client_t *client);
honch_status_t honch_core_shutdown(honch_client_t *client);
const char *honch_core_get_device_id(honch_client_t *client);
honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
honch_status_t honch_core_get_queue_stats(honch_client_t *client, honch_queue_stats_t *stats);
const char *honch_status_string(honch_status_t status);

#ifdef __cplusplus
}
#endif

#endif
