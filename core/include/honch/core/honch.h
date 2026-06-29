#ifndef HONCH_CORE_HONCH_H
#define HONCH_CORE_HONCH_H

#include "honch/core/config.h"
#include "honch/core/error_detail.h"
#include "honch/core/status.h"
#include "honch/core/wire_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_client honch_client_t;
typedef honch_wire_v2_value_t honch_value_t;
typedef honch_wire_v2_property_t honch_property_t;
typedef honch_wire_v2_map_pair_t honch_map_pair_t;

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
/* Port-facing, not application-facing. A port calls this during its own
 * init/boot sequence after detecting a crash recovered from the previous boot;
 * the core emits one reserved $crash event. Returns HONCH_OK once enqueued (or
 * if a crash was already reported this client lifetime), HONCH_ERROR_NOT_SUPPORTED
 * when crash capture is compiled out, HONCH_ERROR_INVALID_ARGUMENT on bad input. */
honch_status_t honch_core_report_crash(
    honch_client_t *client,
    const honch_crash_report_t *report);
/* Port-facing. A port's error-log hook calls this for each error-level log line;
 * the core emits a bounded, rate-limited, coalesced reserved $error event.
 * Returns HONCH_ERROR_NOT_SUPPORTED when log capture is compiled out. */
honch_status_t honch_core_report_log_error(
    honch_client_t *client,
    const char *component,
    const char *message);
honch_status_t honch_core_set_property(honch_client_t *client, const char *key, honch_value_t value);
honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_core_session_end(honch_client_t *client);
honch_status_t honch_core_tick(honch_client_t *client);
honch_status_t honch_core_flush(honch_client_t *client);
honch_status_t honch_core_pause_uploads(honch_client_t *client);
honch_status_t honch_core_resume_uploads(honch_client_t *client);
honch_status_t honch_core_set_uploads_paused(honch_client_t *client, int paused);
honch_status_t honch_core_reset(honch_client_t *client);
honch_status_t honch_core_shutdown(honch_client_t *client);
const char *honch_core_get_device_id(honch_client_t *client);
honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
honch_status_t honch_core_get_queue_stats(honch_client_t *client, honch_queue_stats_t *stats);
const char *honch_status_string(honch_status_t status);

/* Stable snake_case token for a reason code (e.g. "auth_invalid_key"). Returns
 * "unknown" for an unrecognised value. Never NULL. */
const char *honch_error_reason_string(honch_error_reason_t reason);

/* Format a detail into a single human-readable line in the caller-owned buffer,
 * e.g. "rejected: HTTP 401 — API key invalid or revoked (reason=auth_invalid_key)".
 * Always NUL-terminates when buf_size > 0; truncates safely. Returns the number
 * of bytes written (excluding the NUL), or 0 on NULL/zero-size arguments. */
size_t honch_error_detail_format(const honch_error_detail_t *detail, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
