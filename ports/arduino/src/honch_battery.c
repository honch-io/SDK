#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

int honch_read_battery_level(honch_client_t *client)
{
#if HONCH_ENABLE_BATTERY
    if (client->battery_callback == NULL) {
        return -1;
    }

    return client->battery_callback();
#else
    (void)client;
    return -1;
#endif
}

#if HONCH_ENABLE_BATTERY

honch_status_t honch_emit_battery_low_locked(
    honch_client_t *client,
    int battery_level,
    const honch_auto_properties_snapshot_t *auto_properties,
    honch_lifecycle_queue_tracker_t *lifecycle_tracker)
{
    if (battery_level < 0) {
        return HONCH_OK;
    }

    if (battery_level >= client->battery_low_threshold) {
        client->battery_low_emitted = false;
        return HONCH_OK;
    }

    if (client->battery_low_emitted) {
        return HONCH_OK;
    }

    client->battery_low_emitted = true;
    return honch_track_locked_internal(
        client,
        "$battery_low",
        (const honch_wire_v2_property_t[]) {
            honch_prop("level", honch_i64(battery_level))
        },
        1u,
        NULL,
        0u,
        battery_level,
        false,
        auto_properties,
        lifecycle_tracker);
}

#endif /* HONCH_ENABLE_BATTERY */
