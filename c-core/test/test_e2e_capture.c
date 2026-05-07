#include "honch/honch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void log_status(const char *operation, honch_status_t status)
{
    fprintf(stderr, "%s failed: %s\n", operation, honch_status_string(status));
}

int main(void)
{
    const char *endpoint = getenv("HONCH_E2E_ENDPOINT");
    const char *token = getenv("HONCH_E2E_TOKEN");
    if (endpoint == NULL || endpoint[0] == '\0' || token == NULL || token[0] == '\0') {
        fprintf(stderr, "Skipping: HONCH_E2E_ENDPOINT and HONCH_E2E_TOKEN are required.\n");
        return 77;
    }

    char queue_dir[128];
    snprintf(queue_dir, sizeof(queue_dir), "/tmp/honch-e2e-%ld-XXXXXX", (long)getpid());
    if (mkdtemp(queue_dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    honch_config_t config = {
        .api_key = token,
        .endpoint_url = endpoint,
        .device_id = NULL,
        .device_model = "C Core E2E",
        .firmware_version = "e2e",
        .environment = "e2e",
        .queue_directory = queue_dir,
        .batch_size = 10u,
        .max_queued_events = 100u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 15000u
    };

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        log_status("honch_init", status);
        return 1;
    }

    status = honch_track(client, "c_core_e2e_smoke", "{\"source\":\"c-core-e2e\"}");
    if (status == HONCH_OK) {
        status = honch_flush(client);
    }

    honch_shutdown(client);

    if (status != HONCH_OK) {
        log_status("E2E capture", status);
        return 1;
    }

    return 0;
}
