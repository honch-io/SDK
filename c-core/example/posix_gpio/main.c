#include "honch/honch.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum gpio_edge_mode {
    GPIO_EDGE_RISING,
    GPIO_EDGE_FALLING,
    GPIO_EDGE_BOTH
} gpio_edge_mode_t;

typedef struct gpio_edge_tracker {
    int pin;
    int last_value;
    bool has_sample;
} gpio_edge_tracker_t;

static const char *edge_name(int previous_value, int current_value)
{
    return previous_value == 0 && current_value != 0 ? "rising" : "falling";
}

static bool edge_matches(gpio_edge_mode_t mode, int previous_value, int current_value)
{
    if (previous_value == current_value) {
        return false;
    }
    if (mode == GPIO_EDGE_BOTH) {
        return true;
    }
    if (mode == GPIO_EDGE_RISING) {
        return previous_value == 0 && current_value != 0;
    }
    return previous_value != 0 && current_value == 0;
}

static honch_status_t track_gpio_sample(
    honch_client_t *client,
    gpio_edge_tracker_t *tracker,
    gpio_edge_mode_t mode,
    int value)
{
    int normalized = value == 0 ? 0 : 1;
    if (!tracker->has_sample) {
        tracker->last_value = normalized;
        tracker->has_sample = true;
        return HONCH_OK;
    }

    int previous = tracker->last_value;
    tracker->last_value = normalized;
    if (!edge_matches(mode, previous, normalized)) {
        return HONCH_OK;
    }

    char properties[96];
    snprintf(
        properties,
        sizeof(properties),
        "{\"pin\":%d,\"edge\":\"%s\",\"value\":%d}",
        tracker->pin,
        edge_name(previous, normalized),
        normalized);
    return honch_track(client, "gpio_edge", properties);
}

int main(void)
{
    honch_config_t config = {
        .api_key = "local-dev-key",
        .endpoint_url = "http://127.0.0.1:8765",
        .device_id = NULL,
        .device_model = "Linux GPIO Demo",
        .firmware_version = "dev",
        .environment = "dev",
        .queue_directory = ".honch-posix-gpio-queue",
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

    gpio_edge_tracker_t boot_button = {.pin = 0};
    status = track_gpio_sample(client, &boot_button, GPIO_EDGE_BOTH, 0);
    if (status == HONCH_OK) {
        status = track_gpio_sample(client, &boot_button, GPIO_EDGE_BOTH, 1);
    }
    if (status == HONCH_OK) {
        status = track_gpio_sample(client, &boot_button, GPIO_EDGE_BOTH, 0);
    }
    if (status == HONCH_OK) {
        status = honch_flush(client);
    }

    if (status != HONCH_OK) {
        fprintf(stderr, "GPIO example failed: %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 1;
    }

    printf("GPIO example sent edge events successfully.\n");
    honch_shutdown(client);
    return 0;
}
