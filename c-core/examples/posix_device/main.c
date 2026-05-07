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

    status = honch_identify(client, "local-user-001", "{\"role\":\"developer\"}");
    if (status == HONCH_OK) {
        status = honch_set_property(client, "$session_id", "\"local-diagnostics\"");
    }
    if (status == HONCH_OK) {
        status = honch_track(client, "button_pressed", "{\"button\":\"power\"}");
    }
    if (status == HONCH_OK) {
        status = honch_track(client, "screen_viewed", "{\"screen\":\"diagnostics\"}");
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
