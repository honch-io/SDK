#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HONCH_ENABLE_CRASH_CAPTURE
static const char *honch_crash_kind_source(honch_crash_kind_t kind)
{
    switch (kind) {
    case HONCH_CRASH_KIND_NONE:
        return "none";
    case HONCH_CRASH_KIND_PANIC:
        return "panic";
    case HONCH_CRASH_KIND_WATCHDOG:
        return "watchdog";
    case HONCH_CRASH_KIND_ASSERT:
        return "assert";
    case HONCH_CRASH_KIND_BROWNOUT:
        return "brownout";
    case HONCH_CRASH_KIND_STACK_OVERFLOW:
        return "stack_overflow";
    case HONCH_CRASH_KIND_HARDFAULT:
        return "hardfault";
    case HONCH_CRASH_KIND_LOCKUP:
        return "lockup";
    case HONCH_CRASH_KIND_EXCEPTION:
        return "exception";
    case HONCH_CRASH_KIND_SIGNAL:
        return "signal";
    case HONCH_CRASH_KIND_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *honch_crash_severity_string(honch_crash_severity_t severity)
{
    switch (severity) {
    case HONCH_CRASH_SEVERITY_INFO:
        return "info";
    case HONCH_CRASH_SEVERITY_WARNING:
        return "warning";
    case HONCH_CRASH_SEVERITY_FATAL:
        return "fatal";
    default:
        return "fatal";
    }
}

static bool honch_crash_report_is_abnormal(const honch_crash_report_t *crash_report)
{
    return crash_report != NULL && crash_report->kind != HONCH_CRASH_KIND_NONE;
}

/* Append an optional crash string property: skipped silently when the value is
 * absent, blank, or longer than max_length (crash fields are all best-effort). */
static honch_status_t honch_append_crash_string(
    honch_wire_v2_property_t *properties,
    size_t *property_count,
    const char *key,
    const char *value,
    size_t max_length)
{
    size_t length = 0u;
    if (!honch_fault_string_length(value, max_length, &length)) {
        return HONCH_OK;
    }
    return honch_append_typed_property(
        properties,
        property_count,
        key,
        honch_strn(value, length),
        true);
}

/* Build the property set for a $crash event from a port-supplied report. All
 * keys are SDK-owned; the API takes no user properties, so owned-key protection
 * is structural — nothing the caller passes can shadow these. */
static honch_status_t honch_build_crash_properties(
    const honch_crash_report_t *report,
    const char *crash_id,
    honch_wire_v2_property_t *properties,
    size_t *property_count)
{
    *property_count = 0u;
    honch_status_t status = honch_append_typed_property(
        properties, property_count, "source",
        honch_str(honch_crash_kind_source(report->kind)), true);
    if (status == HONCH_OK) {
        status = honch_append_typed_property(
            properties, property_count, "severity",
            honch_str(honch_crash_severity_string(report->severity)), true);
    }
    if (status == HONCH_OK) {
        status = honch_append_typed_property(
            properties, property_count, "reset_reason",
            honch_boot_reset_reason_value(report), true);
    }
    if (status == HONCH_OK && report->summary_version > 0u) {
        status = honch_append_typed_property(
            properties, property_count, "summary_version",
            honch_u64((uint64_t)report->summary_version), true);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "message", report->message, HONCH_FAULT_MESSAGE_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "component", report->component, HONCH_FAULT_COMPONENT_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "firmware_build_id", report->firmware_build_id, HONCH_FAULT_BUILD_ID_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "exception_cause", report->exception_cause, HONCH_FAULT_EXCEPTION_CAUSE_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "fault_pc", report->fault_pc, HONCH_FAULT_PC_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "fault_addr", report->fault_addr, HONCH_FAULT_PC_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "backtrace", report->backtrace, HONCH_FAULT_BACKTRACE_MAX_BYTES);
    }
    if (status == HONCH_OK) {
        status = honch_append_crash_string(properties, property_count,
            "task_name", report->task_name, HONCH_FAULT_TASK_NAME_MAX_BYTES);
    }
    if (status == HONCH_OK && report->coredump_available) {
        status = honch_append_typed_property(
            properties, property_count, "coredump_available", honch_bool(true), true);
    }
    /* Links this $crash summary to its uploaded coredump blob (same id on the
     * blob's chunks). Present whenever a crash_id was generated for this report. */
    if (status == HONCH_OK && crash_id != NULL && crash_id[0] != '\0') {
        status = honch_append_typed_property(
            properties, property_count, "crash_id", honch_str(crash_id), true);
    }
    return status;
}

/* Emit the reserved $crash event for a recovered crash. Used by the init path
 * (lifecycle_tracker set, runs single-threaded during construction) — it
 * prepares its own event context. Once-only across the client lifetime. */
honch_status_t honch_emit_crash_locked(
    honch_client_t *client,
    const honch_crash_report_t *crash_report,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    if (!honch_crash_report_is_abnormal(crash_report) || client->crash_reported) {
        return HONCH_OK;
    }

    /* Generate the crash_id that links this $crash summary to its coredump blob,
     * into a local; commit it to the client only once the $crash is actually
     * emitted, so a no-op re-report can't clobber the live crash's id. */
    char crash_id[33];
    if (honch_client_random_hex(client, crash_id) != HONCH_OK) {
        crash_id[0] = '\0';
    }
    honch_wire_v2_property_t properties[15];
    size_t property_count = 0u;
    honch_status_t status = honch_build_crash_properties(
        crash_report, crash_id, properties, &property_count);
    if (status != HONCH_OK) {
        return HONCH_OK;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    /* The $crash is the first push in this call, so it is assigned the current
     * sequence value; capture it for erase-after-ack delivery tracking. */
    uint64_t crash_sequence = client->sequence;
    if (status == HONCH_OK) {
        status = honch_track_locked_internal(
            client,
            "$crash",
            properties,
            property_count,
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            lifecycle_tracker);
    }
    honch_event_context_free(&event_context);
    if (status == HONCH_OK) {
        client->crash_reported = true;
        client->crash_pending_ack = true;
        client->crash_event_sequence = crash_sequence;
        memcpy(client->coredump_crash_id, crash_id, sizeof(client->coredump_crash_id));
    }
    return status == HONCH_ERROR_INVALID_ARGUMENT || status == HONCH_ERROR_REJECTED ?
        HONCH_OK :
        status;
}
#endif /* HONCH_ENABLE_CRASH_CAPTURE */

honch_status_t honch_core_report_crash(
    honch_client_t *client,
    const honch_crash_report_t *report)
{
#if !HONCH_ENABLE_CRASH_CAPTURE
    (void)report;
    honch_status_t disabled_status = honch_client_enter(client);
    if (disabled_status != HONCH_OK) {
        return disabled_status;
    }
    honch_client_leave(client);
    return HONCH_ERROR_NOT_SUPPORTED;
#else
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }
    if (report == NULL) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    /* Nothing to report, or a crash was already reported this lifetime. */
    if (!honch_crash_report_is_abnormal(report) || client->crash_reported) {
        honch_client_leave(client);
        return HONCH_OK;
    }

    /* Local crash_id; committed to the client only once the $crash is emitted
     * (under the lock), so a racing no-op re-report can't clobber the live id. */
    char crash_id[33];
    if (honch_client_random_hex(client, crash_id) != HONCH_OK) {
        crash_id[0] = '\0';
    }
    honch_wire_v2_property_t properties[15];
    size_t property_count = 0u;
    status = honch_build_crash_properties(report, crash_id, properties, &property_count);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return status;
    }

    /* Re-check once-only under the lock: two callers can pass the pre-lock check
     * concurrently, so the authoritative guard must hold the state lock. */
    if (client->crash_reported) {
        honch_client_unlock(client);
        honch_event_context_free(&event_context);
        honch_client_leave(client);
        return HONCH_OK;
    }

    /* The $crash is the first push under the lock, so it takes the current
     * sequence; capture it for erase-after-ack delivery tracking. */
    uint64_t crash_sequence = client->sequence;
    status = honch_track_locked_internal(
        client,
        "$crash",
        properties,
        property_count,
        NULL,
        0u,
        event_context.battery_level,
        true,
        &event_context.auto_properties,
        NULL);
    if (status == HONCH_OK) {
        client->crash_reported = true;
        client->crash_pending_ack = true;
        client->crash_event_sequence = crash_sequence;
        memcpy(client->coredump_crash_id, crash_id, sizeof(client->coredump_crash_id));
    }
    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
#endif
}

#if HONCH_ENABLE_LOG_CAPTURE
/* SDK-internal log tag, so the log hook never reports the SDK's own logs as
 * $error events (recursion guard). */
#define HONCH_LOG_SELF_TAG "honch"

static void honch_copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0u) {
        return;
    }
    size_t i = 0u;
    if (src != NULL) {
        for (; i + 1u < dst_size && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static uint32_t honch_log_hash(const char *component, const char *message)
{
    uint32_t hash = 2166136261u;
    for (const char *p = component; p != NULL && *p != '\0'; p++) {
        hash = (hash ^ (uint8_t)*p) * 16777619u;
    }
    hash = (hash ^ 0xffu) * 16777619u; /* component/message separator */
    for (const char *p = message; p != NULL && *p != '\0'; p++) {
        hash = (hash ^ (uint8_t)*p) * 16777619u;
    }
    return hash;
}

static void honch_log_accumulate_locked(
    honch_client_t *client,
    const char *component,
    const char *message)
{
    char component_buffer[HONCH_LOG_COMPONENT_STORE_BYTES + 1u];
    char message_buffer[HONCH_LOG_MESSAGE_STORE_BYTES + 1u];
    honch_copy_bounded(component_buffer, sizeof(component_buffer), component);
    honch_copy_bounded(message_buffer, sizeof(message_buffer), message);
    uint32_t hash = honch_log_hash(component_buffer, message_buffer);

    honch_log_error_slot_t *free_slot = NULL;
    for (size_t i = 0u; i < HONCH_LOG_DEDUP_SLOTS; i++) {
        honch_log_error_slot_t *slot = &client->log_error_slots[i];
        if (!slot->active) {
            if (free_slot == NULL) {
                free_slot = slot;
            }
            continue;
        }
        if (slot->hash == hash &&
            strcmp(slot->component, component_buffer) == 0 &&
            strcmp(slot->message, message_buffer) == 0) {
            if (slot->count < UINT32_MAX) {
                slot->count++;
            }
            return;
        }
    }

    if (free_slot == NULL) {
        if (client->log_errors_dropped < UINT32_MAX) {
            client->log_errors_dropped++;
        }
        return;
    }

    free_slot->active = true;
    free_slot->hash = hash;
    free_slot->count = 1u;
    honch_copy_bounded(free_slot->component, sizeof(free_slot->component), component_buffer);
    honch_copy_bounded(free_slot->message, sizeof(free_slot->message), message_buffer);
}

/* Drain the coalesced log-error table into the queue as $error events. Called
 * under the client lock at the start of flush/tick. Uses a minimal event
 * context (no per-event auto properties, no battery read) so it never invokes
 * the battery callback while the lock is held. */
void honch_drain_log_errors_locked(honch_client_t *client)
{
    for (size_t i = 0u; i < HONCH_LOG_DEDUP_SLOTS; i++) {
        honch_log_error_slot_t *slot = &client->log_error_slots[i];
        if (!slot->active) {
            continue;
        }

        honch_wire_v2_property_t properties[5];
        size_t property_count = 0u;
        honch_status_t status = honch_append_typed_property(
            properties, &property_count, "level", honch_str("error"), true);
        if (status == HONCH_OK && slot->component[0] != '\0') {
            status = honch_append_typed_property(
                properties, &property_count, "component", honch_str(slot->component), true);
        }
        if (status == HONCH_OK) {
            status = honch_append_typed_property(
                properties, &property_count, "message", honch_str(slot->message), true);
        }
        if (status == HONCH_OK) {
            status = honch_append_typed_property(
                properties, &property_count, "count", honch_u64((uint64_t)slot->count), true);
        }
        /* Surface distinct errors that overflowed the dedup table (and were never
         * captured) on the first drained $error, so silent drops are observable. */
        if (status == HONCH_OK && client->log_errors_dropped > 0u) {
            status = honch_append_typed_property(
                properties, &property_count, "dropped",
                honch_u64((uint64_t)client->log_errors_dropped), true);
            if (status == HONCH_OK) {
                client->log_errors_dropped = 0u;
            }
        }
        if (status == HONCH_OK) {
            (void)honch_track_locked_internal(
                client, "$error", properties, property_count, NULL, 0u, -1, false, NULL, NULL);
        }
        *slot = (honch_log_error_slot_t){0};
    }
}
#endif /* HONCH_ENABLE_LOG_CAPTURE */

honch_status_t honch_core_report_log_error(
    honch_client_t *client,
    const char *component,
    const char *message)
{
#if !HONCH_ENABLE_LOG_CAPTURE
    (void)component;
    (void)message;
    honch_status_t disabled_status = honch_client_enter(client);
    if (disabled_status != HONCH_OK) {
        return disabled_status;
    }
    honch_client_leave(client);
    return HONCH_ERROR_NOT_SUPPORTED;
#else
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }
    if (honch_is_blank(message)) {
        honch_client_leave(client);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    /* Recursion guard: never turn the SDK's own error logs into $error events. */
    if (component != NULL && strcmp(component, HONCH_LOG_SELF_TAG) == 0) {
        honch_client_leave(client);
        return HONCH_OK;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }
    honch_log_accumulate_locked(client, component, message);
    honch_client_unlock(client);
    honch_client_leave(client);
    return HONCH_OK;
#endif
}
