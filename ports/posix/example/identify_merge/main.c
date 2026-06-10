#include "honch/honch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int check_status(const char *operation, honch_status_t status, honch_client_t *client)
{
    if (status == HONCH_OK) {
        return 0;
    }
    fprintf(stderr, "%s failed: %s\n", operation, honch_status_string(status));
    honch_shutdown(client);
    return 1;
}

int main(void)
{
    const char *api_key = required_env("HONCH_API_KEY");
    const char *endpoint_url = env_or_default("HONCH_CAPTURE_ENDPOINT", "http://127.0.0.1:8001");
    const char *queue_directory = env_or_default("HONCH_QUEUE_DIRECTORY", ".honch-identify-merge-queue");
    const char *run_id_env = getenv("HONCH_EXAMPLE_RUN_ID");

    if (api_key == NULL) {
        fprintf(stderr, "HONCH_API_KEY is required.\n");
        return 1;
    }

    char run_id[64];
    if (run_id_env != NULL && run_id_env[0] != '\0') {
        snprintf(run_id, sizeof(run_id), "%s", run_id_env);
    } else {
        snprintf(run_id, sizeof(run_id), "posix-%ld", (long)time(NULL));
    }

    char user_id[96];
    char pre_event[96];
    char post_event[96];
    snprintf(user_id, sizeof(user_id), "identify-user-%s", run_id);
    snprintf(pre_event, sizeof(pre_event), "identify_pre_%s", run_id);
    snprintf(post_event, sizeof(post_event), "identify_post_%s", run_id);

    honch_config_t config = {
        .api_key = api_key,
        .endpoint_url = endpoint_url,
        .device_id = NULL,
        .device_model = "Identify Merge POSIX Example",
        .firmware_version = "identify-merge-example",
        .environment = "dev",
        .queue_directory = queue_directory,
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

    const char *device_id = honch_get_device_id(client);
    printf("RUN_ID=%s\n", run_id);
    printf("DEVICE_ID=%s\n", device_id != NULL ? device_id : "");
    printf("USER_ID=%s\n", user_id);
    printf("PRE_EVENT=%s\n", pre_event);
    printf("POST_EVENT=%s\n", post_event);

    const honch_property_t pre_properties[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("phase", honch_str("before_identify"))
    };
    status = honch_track(client, pre_event, pre_properties, 2u);
    if (check_status("honch_track(pre)", status, client) != 0) {
        return 1;
    }

    const honch_property_t traits[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("pilot", honch_str("stickercam"))
    };
    status = honch_identify(client, user_id, traits, 2u);
    if (check_status("honch_identify", status, client) != 0) {
        return 1;
    }

    const honch_property_t post_properties[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("phase", honch_str("after_identify"))
    };
    status = honch_track(client, post_event, post_properties, 2u);
    if (check_status("honch_track(post)", status, client) != 0) {
        return 1;
    }

    status = honch_flush(client);
    if (check_status("honch_flush", status, client) != 0) {
        return 1;
    }

    honch_shutdown(client);
    printf("identify merge example sent events successfully\n");
    return 0;
}
