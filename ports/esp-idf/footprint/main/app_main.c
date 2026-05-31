// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if FOOTPRINT_ENABLE_HONCH
#include "honch.h"
#endif

static const char *TAG = "honch_footprint";

#define FOOTPRINT_CPU_SAMPLES 32

#if FOOTPRINT_ENABLE_HONCH
static uint8_t s_event_buffer[32768];
#endif

typedef struct {
    int64_t samples[FOOTPRINT_CPU_SAMPLES];
    int64_t total_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t count;
    uint32_t failures;
} timing_t;

static void log_runtime(const char *phase)
{
    ESP_LOGI(TAG,
             "HONCH_FOOTPRINT_RUNTIME phase=%s heap_free=%" PRIu32
             " heap_min=%" PRIu32 " heap_largest=%" PRIu32
             " stack_high_water=%" PRIu32,
             phase,
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (uint32_t)uxTaskGetStackHighWaterMark(NULL));
}

#if FOOTPRINT_ENABLE_HONCH
static void timing_add(timing_t *timing, int64_t elapsed_us, bool ok)
{
    if (timing->count < FOOTPRINT_CPU_SAMPLES) {
        timing->samples[timing->count] = elapsed_us;
    }
    if (timing->count == 0 || elapsed_us < timing->min_us) {
        timing->min_us = elapsed_us;
    }
    if (elapsed_us > timing->max_us) {
        timing->max_us = elapsed_us;
    }
    timing->total_us += elapsed_us;
    timing->count++;
    if (!ok) {
        timing->failures++;
    }
}

static void sort_samples(int64_t *samples, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        int64_t value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
}
#endif

#if FOOTPRINT_CONNECTED_BASELINE && !FOOTPRINT_ENABLE_HONCH
static void run_connected_baseline_sample(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t event_err = esp_event_loop_create_default();
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_err);
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&wifi_cfg);
    if (wifi_err == ESP_OK) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    } else if (wifi_err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_ERROR_CHECK(wifi_err);
    }

    esp_http_client_config_t http_cfg = {
        .url = "http://127.0.0.1:1/capture",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 1,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/vnd.honch.chunk");
        esp_http_client_set_post_field(client, "baseline", 8);
        esp_http_client_cleanup(client);
    }

    ESP_LOGI(TAG, "HONCH_FOOTPRINT_CONNECTED_BASELINE initialized=1");
}
#endif

#if FOOTPRINT_ENABLE_HONCH
static void run_representative_api_sample(void)
{
    const char *device_id = honch_get_device_id();
    ESP_LOGI(TAG, "HONCH_FOOTPRINT_DEVICE_ID value=%s", device_id ? device_id : "null");

    const honch_property_t traits[] = {
        honch_prop("plan", honch_str("landing-footprint"))
    };
    (void)honch_identify("footprint-user", traits, 1u);
    (void)honch_set_property("firmware_channel", honch_str("footprint"));
    (void)honch_session_start("footprint-session");
    (void)honch_session_end();
}

static void run_honch_cpu_sample(void)
{
    timing_t timing = {0};

    for (int i = 0; i < FOOTPRINT_CPU_SAMPLES; i++) {
        const honch_property_t properties[] = {
            honch_prop("seq", honch_i64(i)),
            honch_prop("kind", honch_str("footprint"))
        };

        int64_t start_us = esp_timer_get_time();
        honch_err_t err = honch_track("footprint_event", properties, 2u);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        timing_add(&timing, elapsed_us, err == HONCH_OK);
    }

    size_t sortable_count = timing.count;
    if (sortable_count > FOOTPRINT_CPU_SAMPLES) {
        sortable_count = FOOTPRINT_CPU_SAMPLES;
    }
    sort_samples(timing.samples, sortable_count);

    int64_t avg_us = timing.count == 0 ? 0 : timing.total_us / timing.count;
    int64_t p50_us = sortable_count == 0 ? 0 : timing.samples[sortable_count / 2];
    size_t p95_index = sortable_count == 0 ? 0 : ((sortable_count * 95) / 100);
    if (p95_index >= sortable_count && sortable_count > 0) {
        p95_index = sortable_count - 1;
    }
    int64_t p95_us = sortable_count == 0 ? 0 : timing.samples[p95_index];
    double cpu_pct_1hz = ((double)avg_us / 1000000.0) * 100.0;

    ESP_LOGI(TAG,
             "HONCH_FOOTPRINT_CPU samples=%" PRIu32 " failures=%" PRIu32
             " avg_us=%" PRId64 " min_us=%" PRId64 " p50_us=%" PRId64
             " p95_us=%" PRId64 " max_us=%" PRId64 " cpu_pct_1hz=%.6f",
             timing.count,
             timing.failures,
             avg_us,
             timing.min_us,
             p50_us,
             p95_us,
             timing.max_us,
             cpu_pct_1hz);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG,
             "HONCH_FOOTPRINT_BUILD with_honch=%d connected_baseline=%d",
             FOOTPRINT_ENABLE_HONCH,
             FOOTPRINT_CONNECTED_BASELINE);
    log_runtime("boot");

#if FOOTPRINT_ENABLE_HONCH
    log_runtime("before_honch");
    honch_config_t config = {
        .api_key = "footprint_api_key",
        .host = "http://127.0.0.1:1",
        .device_model = "footprint-board",
        .firmware_version = "footprint-0.1.0",
        .event_buffer = s_event_buffer,
        .event_buffer_size = sizeof(s_event_buffer),
        .flush_interval_seconds = 3600,
        .flush_event_threshold = 10000,
    };

    honch_err_t err = honch_init(&config);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "HONCH_FOOTPRINT_INIT_ERROR err=%d", err);
    }
    log_runtime("after_honch_init");
    run_honch_cpu_sample();
    log_runtime("after_track_sample");
    run_representative_api_sample();
    log_runtime("after_representative_api");
#elif FOOTPRINT_CONNECTED_BASELINE
    log_runtime("before_connected_baseline");
    run_connected_baseline_sample();
    log_runtime("after_connected_baseline");
#else
    log_runtime("bare_idle");
#endif

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
