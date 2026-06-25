#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

#include <stdlib.h>

#if HONCH_ENABLE_LIFECYCLE_EVENTS

static honch_status_t honch_emit_firmware_update_locked(
    honch_client_t *client,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker,
    bool *firmware_version_pending_save)
{
    if (firmware_version_pending_save != NULL) {
        *firmware_version_pending_save = false;
    }

    bool changed = false;
    char *previous_version = NULL;
    honch_status_t status = honch_state_check_firmware_version(client, &changed, &previous_version);
    if (status != HONCH_OK) {
        free(previous_version);
        return status;
    }
    if (!changed) {
        free(previous_version);
        return honch_state_save_firmware_version(client);
    }

    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status == HONCH_OK) {
        const honch_wire_v2_property_t properties[] = {
            honch_prop("previous_version", honch_str(previous_version)),
            honch_prop("new_version", honch_str(client->firmware_version))
        };
        status = honch_track_locked_internal(
            client,
            "$firmware_update",
            properties,
            sizeof(properties) / sizeof(properties[0]),
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            lifecycle_tracker);
    }
    honch_event_context_free(&event_context);
    if (status == HONCH_OK) {
        if (firmware_version_pending_save != NULL) {
            *firmware_version_pending_save = true;
        } else {
            status = honch_state_save_firmware_version(client);
        }
    }

    free(previous_version);
    return status;
}

honch_status_t honch_lifecycle_emit_boot_locked(
    honch_client_t *client,
    const honch_core_config_t *config,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker,
    bool *firmware_version_pending_save)
{
    honch_status_t status = honch_emit_firmware_update_locked(client, lifecycle_tracker, firmware_version_pending_save);
    if (status != HONCH_OK) {
        return status;
    }
    honch_event_context_t event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &event_context);
    if (status == HONCH_OK) {
        const honch_wire_v2_property_t boot_properties[] = {
            honch_prop("reset_reason", honch_boot_reset_reason_value(config->crash_report))
        };
        status = honch_track_locked_internal(
            client,
            "$device_boot",
            boot_properties,
            sizeof(boot_properties) / sizeof(boot_properties[0]),
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            lifecycle_tracker);
    }
    honch_event_context_free(&event_context);
    return status;
}

honch_status_t honch_lifecycle_emit_shutdown_locked(
    honch_client_t *client,
    honch_event_context_t *event_context)
{
    return honch_track_locked_internal(
        client,
        "$device_shutdown",
        NULL,
        0u,
        NULL,
        0u,
        event_context->battery_level,
        true,
        &event_context->auto_properties,
        NULL);
}

#endif /* HONCH_ENABLE_LIFECYCLE_EVENTS */
