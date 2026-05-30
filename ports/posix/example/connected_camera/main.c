#include "honch/honch.h"

#include <stdlib.h>
#include <stdio.h>

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    if (value != NULL && value[0] != '\0') {
        return value;
    }
    return fallback;
}

static const char *required_env(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    return value;
}

static int send_event(
    honch_client_t *client,
    const char *name,
    const honch_property_t *properties,
    size_t property_count)
{
    printf("camera: %s\n", name);
    honch_status_t status = honch_track(client, name, properties, property_count);
    if (status != HONCH_OK) {
        fprintf(stderr, "honch_track(%s) failed: %s\n", name, honch_status_string(status));
        return 1;
    }
    return 0;
}

int main(void)
{
    const char *api_key = required_env("HONCH_API_KEY");
    const char *endpoint_url = env_or_default("HONCH_CAPTURE_ENDPOINT", "http://127.0.0.1:8001");

    if (api_key == NULL) {
        fprintf(stderr, "HONCH_API_KEY is required.\n");
        return 1;
    }

    honch_config_t config = {
        .api_key = api_key,
        .endpoint_url = endpoint_url,
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

    const honch_property_t traits[] = {
        honch_prop("plan", honch_str("field_trial"))
    };
    status = honch_identify(client, "user-camera-demo-001", traits, 1u);
    if (status == HONCH_OK) {
        status = honch_session_start(client, "recording");
    }
    if (status == HONCH_OK) {
        status = honch_set_property(client, "firmware_channel", honch_str("beta"));
    }
    if (status != HONCH_OK) {
        fprintf(stderr, "camera setup failed: %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 1;
    }

    const honch_property_t powered_on[] = {
        honch_prop("battery_level", honch_i64(87))
    };
    const honch_property_t recording_started[] = {
        honch_prop("mode", honch_str("hdr")),
        honch_prop("resolution", honch_str("4k")),
        honch_prop("fps", honch_i64(60))
    };
    const honch_property_t recording_stopped[] = {
        honch_prop("duration_seconds", honch_i64(42)),
        honch_prop("storage_used_mb", honch_i64(512))
    };
    const honch_property_t battery_warning[] = {
        honch_prop("battery_level", honch_i64(12))
    };
    if (send_event(client, "device_powered_on", powered_on, 1u) != 0 ||
        send_event(client, "recording_started", recording_started, 3u) != 0 ||
        send_event(client, "recording_stopped", recording_stopped, 2u) != 0 ||
        send_event(client, "battery_warning", battery_warning, 1u) != 0) {
        honch_shutdown(client);
        return 1;
    }

    status = honch_session_end(client);
    if (status != HONCH_OK) {
        fprintf(stderr, "honch_session_end failed: %s\n", honch_status_string(status));
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
