#include "honch/honch.h"

#include <stdio.h>

int main(void)
{
    honch_config_t config = {
        .api_key = "local-dev-key",
        .endpoint_url = "http://127.0.0.1:8765",
        .device_id = NULL,
        .device_model = "Mac Dev Rig",
        .firmware_version = "dev",
        .environment = "dev",
        .queue_directory = ".honch-queue",
        .batch_size = 10u,
        .max_queued_events = 100u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 10000u
    };

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        fprintf(stderr, "honch_init failed: %s\n", honch_status_string(status));
        return 1;
    }

    const honch_property_t traits[] = {
        honch_prop("role", honch_str("developer"))
    };
    status = honch_identify(client, "local-user-001", traits, 1u);
    if (status == HONCH_OK) {
        status = honch_session_start(client, "local-diagnostics");
    }
    if (status == HONCH_OK) {
        const honch_property_t properties[] = {
            honch_prop("button", honch_str("power"))
        };
        status = honch_track(client, "button_pressed", properties, 1u);
    }
    if (status == HONCH_OK) {
        const honch_property_t properties[] = {
            honch_prop("screen", honch_str("diagnostics"))
        };
        status = honch_track(client, "screen_viewed", properties, 1u);
    }
    if (status == HONCH_OK) {
        status = honch_session_end(client);
    }
    if (status == HONCH_OK) {
        status = honch_flush(client);
    }

    if (status != HONCH_OK) {
        fprintf(stderr, "Honch example failed: %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 1;
    }

    printf("Honch example sent events successfully.\n");
    honch_shutdown(client);
    return 0;
}
