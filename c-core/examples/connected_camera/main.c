#include "honch/honch.h"

#include <stdio.h>

static int send_event(honch_client_t *client, const char *name, const char *properties_json)
{
    printf("camera: %s\n", name);
    honch_status_t status = honch_track(client, name, properties_json);
    if (status != HONCH_OK) {
        fprintf(stderr, "honch_track(%s) failed: %s\n", name, honch_status_string(status));
        return 1;
    }
    return 0;
}

int main(void)
{
    honch_config_t config = {
        .api_key = "local-dev-key",
        .endpoint_url = "http://127.0.0.1:8765",
        .device_id = NULL,
        .device_model = "ActionCam X1",
        .firmware_version = "1.2.3-dev",
        .environment = "dev",
        .queue_directory = ".honch-connected-camera-queue",
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

    printf("camera: boot\n");

    status = honch_identify(client, "user-camera-demo-001", "{\"plan\":\"field_trial\"}");
    if (status == HONCH_OK) {
        status = honch_set_property(client, "$session_id", "\"recording-session-001\"");
    }
    if (status == HONCH_OK) {
        status = honch_set_property(client, "$firmware_channel", "\"beta\"");
    }
    if (status != HONCH_OK) {
        fprintf(stderr, "camera setup failed: %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 1;
    }

    if (send_event(client, "device_powered_on", "{\"battery_level\":87}") != 0 ||
        send_event(client, "recording_started", "{\"mode\":\"hdr\",\"resolution\":\"4k\",\"fps\":60}") != 0 ||
        send_event(client, "recording_stopped", "{\"duration_seconds\":42,\"storage_used_mb\":512}") != 0 ||
        send_event(client, "battery_warning", "{\"battery_level\":12}") != 0) {
        honch_shutdown(client);
        return 1;
    }

    status = honch_flush(client);
    if (status != HONCH_OK) {
        fprintf(stderr, "honch_flush failed: %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 1;
    }

    printf("camera: sent demo events successfully\n");
    honch_shutdown(client);
    return 0;
}
