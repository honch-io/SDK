#ifndef HONCH_CORE_CONFIG_H
#define HONCH_CORE_CONFIG_H

#include <stddef.h>

#include "honch/core/coredump.h"
#include "honch/core/platform.h"
#include "honch/core/status.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"
#include "honch/core/wire_v2.h"

#define HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS 0xffffffffu

/* Automatic error/crash reporting is on by default and compiled out at build
 * time, never via a runtime flag. HONCH_ENABLE_ERROR_TRACKING is the umbrella;
 * the two sub-toggles let a port strip crash capture or log capture
 * independently (both default to the umbrella). */
#ifndef HONCH_ENABLE_ERROR_TRACKING
#define HONCH_ENABLE_ERROR_TRACKING 1
#endif
#ifndef HONCH_ENABLE_CRASH_CAPTURE
#define HONCH_ENABLE_CRASH_CAPTURE HONCH_ENABLE_ERROR_TRACKING
#endif
#ifndef HONCH_ENABLE_LOG_CAPTURE
#define HONCH_ENABLE_LOG_CAPTURE HONCH_ENABLE_ERROR_TRACKING
#endif

/* Auto-emitted process-lifecycle events ($device_boot, $firmware_update,
 * $device_shutdown) are on by default and compiled out at build time. A port
 * that only wants explicit track() calls can strip them; the firmware-version
 * baseline is still maintained so re-enabling never spuriously re-emits. */
#ifndef HONCH_ENABLE_LIFECYCLE_EVENTS
#define HONCH_ENABLE_LIFECYCLE_EVENTS 1
#endif

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

typedef enum honch_crash_kind {
    HONCH_CRASH_KIND_NONE = 0,
    HONCH_CRASH_KIND_PANIC = 1,
    HONCH_CRASH_KIND_WATCHDOG = 2,
    HONCH_CRASH_KIND_ASSERT = 3,
    HONCH_CRASH_KIND_BROWNOUT = 4,
    HONCH_CRASH_KIND_STACK_OVERFLOW = 5,
    HONCH_CRASH_KIND_HARDFAULT = 6,
    HONCH_CRASH_KIND_LOCKUP = 7,
    HONCH_CRASH_KIND_EXCEPTION = 8,
    HONCH_CRASH_KIND_SIGNAL = 9,
    HONCH_CRASH_KIND_UNKNOWN = 10
} honch_crash_kind_t;

typedef enum honch_crash_severity {
    HONCH_CRASH_SEVERITY_INFO = 0,
    HONCH_CRASH_SEVERITY_WARNING = 1,
    HONCH_CRASH_SEVERITY_FATAL = 2
} honch_crash_severity_t;

/* A crash recovered from the previous boot, built by a port from its platform's
 * native fault machinery (ESP-IDF coredump summary, an MCU fault handler, a
 * POSIX signal record, ...). Only `kind` is meaningful for every port; richer
 * fields are filled when the platform can provide them — that optionality is
 * how capture is tiered by platform capability. */
typedef struct honch_crash_report {
    honch_crash_kind_t kind;
    honch_crash_severity_t severity;
    const char *reset_reason;
    const char *message;
    const char *component;
    const char *task_name;
    const char *exception_cause;
    const char *fault_pc;
    const char *fault_addr;
    const char *backtrace;
    const char *firmware_build_id;
    unsigned int summary_version;
    int coredump_available;
} honch_crash_report_t;

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
    /* The crash recovered from the previous boot, if any. A port detects the
     * crash, fills this, and points the config at it; the core emits one
     * reserved $crash event during init. NULL means "no crash to report". */
    const honch_crash_report_t *crash_report;
    /* Invoked once after a reported $crash has been delivered to Capture, so the
     * port can clear the on-device crash source (erase-after-ack). Optional.
     * IMPORTANT single-erase rule: this callback and coredump_source->clear must
     * NOT both erase the same backing store. When coredump_source is set the core
     * SUPPRESSES this callback entirely and lets the blob's clear() perform the
     * one and only erase (after the blob's final ack) — the summary is delivered
     * first, so erasing here would wipe the store out from under the in-flight
     * coredump. Use this callback only when no coredump_source is wired. */
    void (*crash_uploaded_callback)(void *userdata);
    void *crash_uploaded_userdata;
    /* Optional: a view over the device's raw coredump image (e.g. the ESP-IDF
     * coredump flash partition). When set and non-empty, the SDK streams the
     * image to Capture as a `coredump` source after a crash, then clears it via
     * the source's clear() on full delivery (or on permanent rejection). This
     * clear() is the SOLE erase of the crash store; see crash_uploaded_callback.
     * NULL = no raw coredump upload. */
    const honch_coredump_source_t *coredump_source;
} honch_core_config_t;

#ifdef __cplusplus
}
#endif

#endif
