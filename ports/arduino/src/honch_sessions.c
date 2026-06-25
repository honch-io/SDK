#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "honch_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HONCH_ENABLE_SESSIONS

static honch_status_t honch_new_session_id(honch_client_t *client, char **out)
{
    char random[33];
    honch_status_t status = honch_client_random_hex(client, random);
    if (status != HONCH_OK) {
        return status;
    }

    char *session_id = (char *)malloc(38u);
    if (session_id == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    snprintf(session_id, 38u, "sess_%s", random);
    *out = session_id;
    return HONCH_OK;
}

honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
        return status;
    }

    char *session_id = NULL;
    status = honch_new_session_id(client, &session_id);
    if (status != HONCH_OK) {
        honch_client_leave(client);
        return status;
    }

    honch_event_context_t end_event_context = {.battery_level = -1};
    honch_event_context_t start_event_context = {.battery_level = -1};
    status = honch_prepare_event_context(client, &end_event_context);
    if (status == HONCH_OK) {
        status = honch_prepare_event_context(client, &start_event_context);
    }
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    status = honch_client_lock(client);
    if (status != HONCH_OK) {
        honch_event_context_free(&end_event_context);
        honch_event_context_free(&start_event_context);
        free(session_id);
        honch_client_leave(client);
        return status;
    }

    honch_lifecycle_queue_tracker_t replacement_tracker;
    honch_lifecycle_queue_tracker_begin(client, &replacement_tracker);
    char *old_session_id = NULL;
    bool replacing_session = client->session_id != NULL;

    if (replacing_session) {
        status = honch_track_locked_internal(
            client,
            "$session_end",
            NULL,
            0u,
            NULL,
            0u,
            end_event_context.battery_level,
            true,
            &end_event_context.auto_properties,
            &replacement_tracker);
        if (status == HONCH_OK) {
            old_session_id = client->session_id;
            client->session_id = NULL;
        }
    }
    if (status == HONCH_OK) {
        client->session_id = session_id;
        session_id = NULL;
        const honch_wire_v2_property_t *start_properties = NULL;
        size_t start_property_count = 0u;
        const honch_wire_v2_property_t named_start_properties[] = {
            honch_prop("session_name", honch_str(session_name))
        };
        if (!honch_is_blank(session_name)) {
            start_properties = named_start_properties;
            start_property_count = 1u;
        }
        status = honch_track_locked_internal(
            client,
            "$session_start",
            start_properties,
            start_property_count,
            NULL,
            0u,
            start_event_context.battery_level,
            true,
            &start_event_context.auto_properties,
            &replacement_tracker);
        if (status != HONCH_OK) {
            free(client->session_id);
            client->session_id = old_session_id;
            old_session_id = NULL;
            if (replacing_session) {
                honch_status_t rollback_status =
                    honch_lifecycle_queue_tracker_rollback(client, &replacement_tracker);
                if (rollback_status != HONCH_OK) {
                    status = rollback_status;
                }
            }
        }
    }
    if (status == HONCH_OK) {
        free(old_session_id);
    }

    honch_client_unlock(client);
    honch_event_context_free(&end_event_context);
    honch_event_context_free(&start_event_context);
    free(session_id);
    free(old_session_id);
    honch_client_leave(client);
    return status;
}

honch_status_t honch_core_session_end(honch_client_t *client)
{
    honch_status_t status = honch_client_enter(client);
    if (status != HONCH_OK) {
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

    if (client->session_id != NULL) {
        status = honch_track_locked_internal(
            client,
            "$session_end",
            NULL,
            0u,
            NULL,
            0u,
            event_context.battery_level,
            true,
            &event_context.auto_properties,
            NULL);
        if (status == HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }

    honch_client_unlock(client);
    honch_event_context_free(&event_context);
    honch_client_leave(client);
    return status;
}

#else /* !HONCH_ENABLE_SESSIONS */

honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name)
{
    (void)session_name;
    honch_status_t disabled_status = honch_client_enter(client);
    if (disabled_status != HONCH_OK) {
        return disabled_status;
    }
    honch_client_leave(client);
    return HONCH_ERROR_NOT_SUPPORTED;
}

honch_status_t honch_core_session_end(honch_client_t *client)
{
    honch_status_t disabled_status = honch_client_enter(client);
    if (disabled_status != HONCH_OK) {
        return disabled_status;
    }
    honch_client_leave(client);
    return HONCH_ERROR_NOT_SUPPORTED;
}

#endif /* HONCH_ENABLE_SESSIONS */
