#ifndef HONCH_CORE_CONFIG_H
#define HONCH_CORE_CONFIG_H

#include <stddef.h>

#include "honch/core/platform.h"
#include "honch/core/status.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"
#include "honch/core/wire_v2.h"

#define HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS 0xffffffffu

#ifdef __cplusplus
extern "C" {
#endif

typedef honch_status_t (*honch_property_sink_fn)(
    void *ctx,
    const char *key,
    honch_wire_v2_value_t value);

typedef honch_status_t (*honch_auto_properties_fn)(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx);

typedef int (*honch_connectivity_fn)(void *userdata);

typedef enum honch_durability_mode {
    HONCH_DURABILITY_OS_BUFFERED = 0,
    HONCH_DURABILITY_SYNC_ALWAYS = 1,
    HONCH_DURABILITY_DEFAULT = HONCH_DURABILITY_OS_BUFFERED
} honch_durability_mode_t;

typedef enum honch_fault_kind {
    HONCH_FAULT_KIND_NONE = 0,
    HONCH_FAULT_KIND_PANIC = 1,
    HONCH_FAULT_KIND_WATCHDOG = 2,
    HONCH_FAULT_KIND_ASSERT = 3,
    HONCH_FAULT_KIND_BROWNOUT = 4,
    HONCH_FAULT_KIND_STACK_OVERFLOW = 5,
    HONCH_FAULT_KIND_UNKNOWN = 6
} honch_fault_kind_t;

typedef enum honch_fault_severity {
    HONCH_FAULT_SEVERITY_INFO = 0,
    HONCH_FAULT_SEVERITY_WARNING = 1,
    HONCH_FAULT_SEVERITY_FATAL = 2
} honch_fault_severity_t;

typedef struct honch_fault_snapshot {
    honch_fault_kind_t kind;
    honch_fault_severity_t severity;
    const char *reset_reason;
    const char *message;
    const char *component;
    unsigned int crash_summary_version;
    const char *firmware_build_id;
    const char *exception_cause;
    const char *fault_pc;
    const char *backtrace;
    const char *task_name;
} honch_fault_snapshot_t;

typedef struct honch_core_config {
    const char *api_key;
    const char *endpoint_url;
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *environment;
    const char *sdk_platform;
    const char *queue_directory;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    unsigned int flush_min_interval_ms;
    size_t flush_event_threshold;
    size_t flush_max_batches;
    size_t shutdown_flush_max_batches;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    int (*battery_callback)(void);
    int battery_low_threshold;
    honch_auto_properties_fn auto_properties_callback;
    void *auto_properties_userdata;
    honch_connectivity_fn connectivity_callback;
    void *connectivity_userdata;
    honch_durability_mode_t durability_mode;
    const honch_platform_ops_t *platform;
    const honch_state_storage_ops_t *state_storage;
    const honch_event_queue_ops_t *event_queue;
    const honch_transport_ops_t *transport;
    const honch_fault_snapshot_t *fault_snapshot;
} honch_core_config_t;

#ifdef __cplusplus
}
#endif

#endif
