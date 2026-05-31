// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "honch.h"

static const char *TAG = "honch_rate_sweep";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5
#define NETWORK_READY_RETRIES 12
#define NETWORK_READY_DELAY_MS 1000
#define NETWORK_READY_TIMEOUT_MS 10000
#define RATE_SWEEP_MAX_RATES 8
#define RATE_SWEEP_MAX_FLUSH_SAMPLES 256
#define CPU_TASK_SNAPSHOT_MAX 48
#define CPU_PERCENT_UNAVAILABLE UINT32_MAX
#define PAD_SOURCE "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

typedef struct {
    uint64_t idle_runtime;
    uint64_t total_runtime;
    bool available;
} cpu_snapshot_t;

typedef struct {
    uint32_t attempted;
    uint32_t accepted;
    uint32_t failed;
    uint32_t flush_count;
    uint32_t flush_failures;
    uint32_t track_count;
    uint32_t flush_sample_count;
    uint64_t track_total_us;
    uint64_t flush_total_us;
    uint32_t track_max_us;
} bench_window_t;

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static uint8_t s_event_buffer[32768];
static uint32_t s_queued_estimate = 0;
static uint32_t s_accepted_since_flush = 0;
static uint32_t s_track_samples[CONFIG_RATE_SWEEP_MAX_WINDOW_SAMPLES];
static uint32_t s_flush_samples[RATE_SWEEP_MAX_FLUSH_SAMPLES];
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
static TaskStatus_t s_task_status[CPU_TASK_SNAPSHOT_MAX];
#endif

static int compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static uint32_t percentile_u32(uint32_t *samples, uint32_t count, uint32_t pct)
{
    if (count == 0) {
        return 0;
    }
    qsort(samples, count, sizeof(samples[0]), compare_u32);
    uint32_t index = ((count - 1u) * pct + 99u) / 100u;
    if (index >= count) {
        index = count - 1u;
    }
    return samples[index];
}

static void delay_until_us(int64_t deadline_us)
{
    while (true) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return;
        }
        if (remaining_us > 2000) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            esp_rom_delay_us((uint32_t)remaining_us);
        }
    }
}

static uint32_t fixed_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0) {
        return 0;
    }
    return (uint32_t)((numerator * 100u) / denominator);
}

static cpu_snapshot_t cpu_snapshot(void)
{
    cpu_snapshot_t snapshot = {0};
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(
        s_task_status,
        CPU_TASK_SNAPSHOT_MAX,
        &total_runtime);
    if (count == 0) {
        return snapshot;
    }

    for (UBaseType_t i = 0; i < count; i++) {
        const char *name = s_task_status[i].pcTaskName;
        if (name != NULL && strncmp(name, "IDLE", 4) == 0) {
            snapshot.idle_runtime += (uint64_t)s_task_status[i].ulRunTimeCounter;
        }
    }
    snapshot.total_runtime = (uint64_t)total_runtime;
    snapshot.available = snapshot.total_runtime > 0;
#endif
    return snapshot;
}

static uint32_t cpu_percent_x100(cpu_snapshot_t before, cpu_snapshot_t after)
{
    if (!before.available || !after.available || after.total_runtime <= before.total_runtime) {
        return CPU_PERCENT_UNAVAILABLE;
    }

    uint64_t total_delta = after.total_runtime - before.total_runtime;
    uint64_t idle_delta = after.idle_runtime >= before.idle_runtime
        ? after.idle_runtime - before.idle_runtime
        : 0;
    uint64_t busy_delta = total_delta > idle_delta ? total_delta - idle_delta : 0;
    return fixed_x100(busy_delta * 100u, total_delta);
}

static void window_add_track_sample(bench_window_t *window, uint32_t elapsed_us)
{
    if (window->track_count < CONFIG_RATE_SWEEP_MAX_WINDOW_SAMPLES) {
        s_track_samples[window->track_count++] = elapsed_us;
    } else {
        window->failed++;
    }
    window->track_total_us += elapsed_us;
    if (elapsed_us > window->track_max_us) {
        window->track_max_us = elapsed_us;
    }
}

static void window_add_flush_sample(bench_window_t *window, uint32_t elapsed_us, bool ok)
{
    if (window->flush_sample_count < RATE_SWEEP_MAX_FLUSH_SAMPLES) {
        s_flush_samples[window->flush_sample_count++] = elapsed_us;
    }
    window->flush_count++;
    window->flush_total_us += elapsed_us;
    if (!ok) {
        window->flush_failures++;
    }
}

static int parse_rate_list(const char *rates_string, uint32_t *rates, size_t max_rates)
{
    char copy[96];
    int written = snprintf(copy, sizeof(copy), "%s", rates_string);
    if (written < 0 || written >= (int)sizeof(copy)) {
        return 0;
    }

    int count = 0;
    char *cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *end = NULL;
        unsigned long parsed = strtoul(cursor, &end, 10);
        if (cursor[0] == '\0' || end == NULL || *end != '\0' || parsed == 0 ||
            parsed > UINT32_MAX || (size_t)count >= max_rates) {
            return 0;
        }
        rates[count++] = (uint32_t)parsed;
        cursor = comma == NULL ? NULL : comma + 1;
    }
    return count;
}

static int current_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to Wi-Fi SSID: %s rssi=%d", CONFIG_WIFI_SSID, current_rssi());
        return true;
    }

    ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
    return false;
}

static void build_health_url(char *url, size_t url_size)
{
    size_t host_len = strlen(CONFIG_HONCH_HOST);
    const char *separator = host_len > 0 && CONFIG_HONCH_HOST[host_len - 1] == '/' ? "" : "/";
    snprintf(url, url_size, "%s%shealth", CONFIG_HONCH_HOST, separator);
}

static bool wait_for_network_ready(void)
{
    char health_url[320];
    build_health_url(health_url, sizeof(health_url));

    ESP_LOGI(TAG, "Waiting for HTTP readiness: %s", health_url);
    for (int attempt = 1; attempt <= NETWORK_READY_RETRIES; attempt++) {
        esp_http_client_config_t client_config = {
            .url = health_url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = NETWORK_READY_TIMEOUT_MS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&client_config);
        if (client == NULL) {
            ESP_LOGW(TAG, "HTTP readiness attempt %d: client init failed", attempt);
        } else {
            esp_err_t err = esp_http_client_perform(client);
            int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
            esp_http_client_cleanup(client);

            if (err == ESP_OK && status > 0) {
                ESP_LOGI(TAG, "HTTP readiness confirmed status=%d attempt=%d", status, attempt);
                vTaskDelay(pdMS_TO_TICKS(500));
                return true;
            }
            ESP_LOGW(
                TAG,
                "HTTP readiness attempt %d/%d failed err=%s status=%d",
                attempt,
                NETWORK_READY_RETRIES,
                esp_err_to_name(err),
                status);
        }
        vTaskDelay(pdMS_TO_TICKS(NETWORK_READY_DELAY_MS));
    }

    ESP_LOGE(TAG, "HTTP readiness failed; benchmark would measure network warm-up");
    return false;
}

static void fill_pad(char *pad, size_t pad_size)
{
    size_t want = CONFIG_RATE_SWEEP_PAYLOAD_BYTES;
    if (want >= pad_size) {
        want = pad_size - 1;
    }

    size_t source_len = strlen(PAD_SOURCE);
    for (size_t i = 0; i < want; i++) {
        pad[i] = PAD_SOURCE[i % source_len];
    }
    pad[want] = '\0';
}

static bool run_rate_sweep_warmup(uint32_t rate, const char *pad)
{
    if (CONFIG_RATE_SWEEP_WARMUP_SECONDS == 0) {
        return true;
    }

    uint32_t sequence = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t end_us = start_us + ((int64_t)CONFIG_RATE_SWEEP_WARMUP_SECONDS * 1000000);

    while (esp_timer_get_time() < end_us) {
        int64_t deadline_us = start_us + (((int64_t)sequence * 1000000) / rate);
        delay_until_us(deadline_us);
        const honch_property_t properties[] = {
            honch_prop("rate", honch_u64(rate)),
            honch_prop("seq", honch_u64(sequence)),
            honch_prop("warmup", honch_bool(true)),
            honch_prop("pad", honch_str(pad))
        };
        honch_err_t track_err = honch_track("bench_rate_warmup", properties, 4u);
        if (track_err != HONCH_OK) {
            return false;
        }
        s_queued_estimate++;
        s_accepted_since_flush++;
        sequence++;

        if (s_accepted_since_flush >= CONFIG_RATE_SWEEP_FLUSH_EVERY) {
            honch_err_t flush_err = honch_flush();
            if (flush_err != HONCH_OK) {
                return false;
            }
            s_queued_estimate = 0;
            s_accepted_since_flush = 0;
        }
    }

    if (s_queued_estimate > 0) {
        honch_err_t flush_err = honch_flush();
        if (flush_err != HONCH_OK) {
            return false;
        }
        s_queued_estimate = 0;
        s_accepted_since_flush = 0;
    }
    return true;
}

static bool run_rate_window(uint32_t rate, uint32_t window_index, const char *pad)
{
    bench_window_t window = {0};
    cpu_snapshot_t cpu_before = cpu_snapshot();
    int64_t window_start_us = esp_timer_get_time();
    int64_t window_end_us = window_start_us + ((int64_t)CONFIG_RATE_SWEEP_WINDOW_SECONDS * 1000000);

    while (esp_timer_get_time() < window_end_us) {
        int64_t deadline_us = window_start_us + (((int64_t)window.attempted * 1000000) / rate);
        delay_until_us(deadline_us);
        uint32_t sequence = window_index * 100000u + window.attempted;
        const honch_property_t properties[] = {
            honch_prop("rate", honch_u64(rate)),
            honch_prop("window", honch_u64(window_index)),
            honch_prop("seq", honch_u64(sequence)),
            honch_prop("sample", honch_u64((sequence * 1103515245u + 12345u) & 0xffffu)),
            honch_prop("pad", honch_str(pad))
        };

        int64_t start_us = esp_timer_get_time();
        honch_err_t track_err = honch_track("bench_rate_event", properties, 5u);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        window_add_track_sample(&window, elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us);
        window.attempted++;

        if (track_err == HONCH_OK) {
            window.accepted++;
            s_queued_estimate++;
            s_accepted_since_flush++;
        } else {
            window.failed++;
        }

        if (s_accepted_since_flush >= CONFIG_RATE_SWEEP_FLUSH_EVERY) {
            start_us = esp_timer_get_time();
            honch_err_t flush_err = honch_flush();
            elapsed_us = esp_timer_get_time() - start_us;
            window_add_flush_sample(
                &window,
                elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us,
                flush_err == HONCH_OK);
            if (flush_err == HONCH_OK) {
                s_queued_estimate = 0;
                s_accepted_since_flush = 0;
            }
        }
    }

    int64_t wall_us = esp_timer_get_time() - window_start_us;
    cpu_snapshot_t cpu_after = cpu_snapshot();
    uint32_t actual_eps_x100 = fixed_x100((uint64_t)window.accepted * 1000000u, (uint64_t)wall_us);
    uint32_t cpu_x100 = cpu_percent_x100(cpu_before, cpu_after);
    char cpu_value[16];
    if (cpu_x100 == CPU_PERCENT_UNAVAILABLE) {
        snprintf(cpu_value, sizeof(cpu_value), "NA");
    } else {
        snprintf(cpu_value, sizeof(cpu_value), "%" PRIu32 ".%02" PRIu32, cpu_x100 / 100u, cpu_x100 % 100u);
    }

    uint32_t track_avg_us = window.track_count == 0
        ? 0
        : (uint32_t)(window.track_total_us / window.track_count);
    uint32_t flush_avg_us = window.flush_sample_count == 0
        ? 0
        : (uint32_t)(window.flush_total_us / window.flush_sample_count);
    uint32_t track_p50_us = percentile_u32(s_track_samples, window.track_count, 50);
    uint32_t track_p95_us = percentile_u32(s_track_samples, window.track_count, 95);
    uint32_t track_p99_us = percentile_u32(s_track_samples, window.track_count, 99);
    uint32_t flush_p95_us = percentile_u32(s_flush_samples, window.flush_sample_count, 95);

    ESP_LOGI(
        TAG,
        "BENCH_WINDOW runtime=esp-idf mode=capture-e2e rate_target_eps=%" PRIu32
        " window=%" PRIu32 " actual_eps=%" PRIu32 ".%02" PRIu32
        " events_attempted=%" PRIu32 " events_accepted=%" PRIu32
        " events_failed=%" PRIu32 " track_avg_us=%" PRIu32
        " track_p50_us=%" PRIu32 " track_p95_us=%" PRIu32
        " track_p99_us=%" PRIu32 " track_max_us=%" PRIu32
        " flush_count=%" PRIu32 " flush_failures=%" PRIu32
        " flush_avg_us=%" PRIu32 " flush_p95_us=%" PRIu32
        " cpu_percent=%s queued_estimate=%" PRIu32
        " heap_free=%" PRIu32 " heap_min=%" PRIu32
        " heap_largest=%" PRIu32 " stack_high_water=%" PRIu32
        " rssi=%d",
        rate,
        window_index,
        actual_eps_x100 / 100u,
        actual_eps_x100 % 100u,
        window.attempted,
        window.accepted,
        window.failed,
        track_avg_us,
        track_p50_us,
        track_p95_us,
        track_p99_us,
        window.track_max_us,
        window.flush_count,
        window.flush_failures,
        flush_avg_us,
        flush_p95_us,
        cpu_value,
        s_queued_estimate,
        (uint32_t)esp_get_free_heap_size(),
        (uint32_t)esp_get_minimum_free_heap_size(),
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (uint32_t)uxTaskGetStackHighWaterMark(NULL),
        current_rssi());

    return window.failed == 0 && window.flush_failures == 0;
}

static bool run_rate_sweep_suite(uint32_t suite_id)
{
    uint32_t rates[RATE_SWEEP_MAX_RATES] = {0};
    int rate_count = parse_rate_list(CONFIG_RATE_SWEEP_RATES, rates, RATE_SWEEP_MAX_RATES);
    if (rate_count <= 0) {
        ESP_LOGE(TAG, "Invalid RATE_SWEEP_RATES: %s", CONFIG_RATE_SWEEP_RATES);
        return false;
    }

    char pad[CONFIG_RATE_SWEEP_PAYLOAD_BYTES + 1];
    fill_pad(pad, sizeof(pad));

    ESP_LOGI(
        TAG,
        "BENCH_SUITE_START schema=honch-rate-sweep-v1 runtime=esp-idf"
        " mode=capture-e2e suite=%" PRIu32 " rates=%s duration_seconds=%d"
        " warmup_seconds=%d window_seconds=%d payload_bytes=%d flush_every=%d",
        suite_id,
        CONFIG_RATE_SWEEP_RATES,
        CONFIG_RATE_SWEEP_DURATION_SECONDS,
        CONFIG_RATE_SWEEP_WARMUP_SECONDS,
        CONFIG_RATE_SWEEP_WINDOW_SECONDS,
        CONFIG_RATE_SWEEP_PAYLOAD_BYTES,
        CONFIG_RATE_SWEEP_FLUSH_EVERY);

    bool ok = true;
    for (int i = 0; i < rate_count; i++) {
        uint32_t rate = rates[i];
        ESP_LOGI(
            TAG,
            "BENCH_RATE_START runtime=esp-idf mode=capture-e2e suite=%" PRIu32
            " rate_target_eps=%" PRIu32 " duration_seconds=%d"
            " warmup_seconds=%d window_seconds=%d",
            suite_id,
            rate,
            CONFIG_RATE_SWEEP_DURATION_SECONDS,
            CONFIG_RATE_SWEEP_WARMUP_SECONDS,
            CONFIG_RATE_SWEEP_WINDOW_SECONDS);

        if (!run_rate_sweep_warmup(rate, pad)) {
            ok = false;
        }

        uint32_t windows = CONFIG_RATE_SWEEP_DURATION_SECONDS / CONFIG_RATE_SWEEP_WINDOW_SECONDS;
        if ((CONFIG_RATE_SWEEP_DURATION_SECONDS % CONFIG_RATE_SWEEP_WINDOW_SECONDS) != 0) {
            windows++;
        }

        for (uint32_t window = 1; ok && window <= windows; window++) {
            ok = run_rate_window(rate, window, pad);
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        int64_t flush_start_us = esp_timer_get_time();
        honch_err_t flush_err = honch_flush();
        int64_t flush_us = esp_timer_get_time() - flush_start_us;
        if (flush_err == HONCH_OK) {
            s_queued_estimate = 0;
            s_accepted_since_flush = 0;
        } else {
            ok = false;
        }
        ESP_LOGI(
            TAG,
            "BENCH_RATE_END runtime=esp-idf mode=capture-e2e suite=%" PRIu32
            " rate_target_eps=%" PRIu32 " status=%s final_flush_us=%" PRId64
            " queued_estimate=%" PRIu32,
            suite_id,
            rate,
            flush_err == HONCH_OK ? "ok" : "failed",
            flush_us,
            s_queued_estimate);
    }

    ESP_LOGI(
        TAG,
        "BENCH_SUITE_END schema=honch-rate-sweep-v1 runtime=esp-idf"
        " mode=capture-e2e suite=%" PRIu32 " status=%s",
        suite_id,
        ok ? "ok" : "failed");
    return ok;
}

void app_main(void)
{
    if (!wifi_init_sta()) {
        return;
    }
    if (!wait_for_network_ready()) {
        return;
    }

    honch_config_t config = {
        .api_key = CONFIG_HONCH_API_KEY,
        .host = CONFIG_HONCH_HOST,
        .device_model = "bench-board",
        .firmware_version = "rate-sweep-bench-0.1.0",
        .event_buffer = s_event_buffer,
        .event_buffer_size = sizeof(s_event_buffer),
        .flush_event_threshold = CONFIG_RATE_SWEEP_FLUSH_EVERY,
    };

    honch_err_t err = honch_init(&config);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Failed to initialize Honch: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Honch initialized, device_id=%s", honch_get_device_id());
    (void)run_rate_sweep_suite(1);

    if (CONFIG_RATE_SWEEP_SETTLE_SECONDS > 0) {
        ESP_LOGI(TAG, "BENCH_SETTLE seconds=%d", CONFIG_RATE_SWEEP_SETTLE_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RATE_SWEEP_SETTLE_SECONDS * 1000));
    }

    ESP_LOGI(TAG, "Rate-sweep benchmark complete.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
