// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "honch.h"
#include "nvs_flash.h"

static const char *TAG = "honch_benchtest";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5
#define BENCH_NETWORK_READY_RETRIES 12
#define BENCH_NETWORK_READY_DELAY_MS 1000
#define BENCH_NETWORK_READY_TIMEOUT_MS 10000
#define PAD_SOURCE "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

#ifndef CONFIG_BENCH_OFFLINE_QUEUE_PROOF
#define CONFIG_BENCH_OFFLINE_QUEUE_PROOF 0
#endif

#ifndef CONFIG_BENCH_OFFLINE_QUEUE_PROOF_EVENTS
#define CONFIG_BENCH_OFFLINE_QUEUE_PROOF_EVENTS 40
#endif

#ifndef CONFIG_BENCH_OFFLINE_RECOVERY_RETRY_SECONDS
#define CONFIG_BENCH_OFFLINE_RECOVERY_RETRY_SECONDS 5
#endif

typedef struct {
    int64_t total_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t count;
    uint32_t failures;
} bench_timer_t;

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static uint8_t s_event_buffer[32768];
static uint32_t s_queued_estimate = 0;

static void timer_add(bench_timer_t *timer, int64_t elapsed_us, bool ok)
{
    if (timer->count == 0 || elapsed_us < timer->min_us) {
        timer->min_us = elapsed_us;
    }
    if (elapsed_us > timer->max_us) {
        timer->max_us = elapsed_us;
    }
    timer->total_us += elapsed_us;
    timer->count++;
    if (!ok) {
        timer->failures++;
    }
}

static int64_t timer_avg_us(const bench_timer_t *timer)
{
    if (timer->count == 0) {
        return 0;
    }
    return timer->total_us / timer->count;
}

static int current_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

static void init_nvs_for_wifi(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
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

    init_nvs_for_wifi();
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

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

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
    for (int attempt = 1; attempt <= BENCH_NETWORK_READY_RETRIES; attempt++) {
        esp_http_client_config_t client_config = {
            .url = health_url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = BENCH_NETWORK_READY_TIMEOUT_MS,
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
            ESP_LOGW(TAG,
                     "HTTP readiness attempt %d/%d failed err=%s status=%d",
                     attempt,
                     BENCH_NETWORK_READY_RETRIES,
                     esp_err_to_name(err),
                     status);
        }

        vTaskDelay(pdMS_TO_TICKS(BENCH_NETWORK_READY_DELAY_MS));
    }

    ESP_LOGE(TAG, "HTTP readiness failed; benchmark would measure network warm-up, not SDK overhead");
    return false;
}

static void fill_pad(char *pad, size_t pad_size)
{
    size_t want = CONFIG_BENCH_PAYLOAD_BYTES;
    if (want >= pad_size) {
        want = pad_size - 1;
    }

    size_t source_len = strlen(PAD_SOURCE);
    for (size_t i = 0; i < want; i++) {
        pad[i] = PAD_SOURCE[i % source_len];
    }
    pad[want] = '\0';
}

static void print_resource_summary(const char *label)
{
    ESP_LOGI(TAG,
             "BENCH_RESOURCE_SUMMARY label=%s heap_free=%" PRIu32
             " heap_min=%" PRIu32 " heap_largest=%" PRIu32
             " stack_high_water=%" PRIu32 " rssi=%d queued_estimate=%" PRIu32,
             label,
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (uint32_t)uxTaskGetStackHighWaterMark(NULL),
             current_rssi(),
             s_queued_estimate);
}

static void run_benchmark_once(uint32_t run_id)
{
    bench_timer_t track_timer = {0};
    bench_timer_t flush_timer = {0};
    char pad[CONFIG_BENCH_PAYLOAD_BYTES + 1];

    fill_pad(pad, sizeof(pad));

    ESP_LOGI(TAG,
             "BENCH_RUN_START run=%" PRIu32 " events=%d interval_ms=%d flush_every=%d"
             " payload_bytes=%d heap_free=%" PRIu32 " heap_min=%" PRIu32 " rssi=%d",
             run_id,
             CONFIG_BENCH_EVENT_COUNT,
             CONFIG_BENCH_EVENT_INTERVAL_MS,
             CONFIG_BENCH_FLUSH_EVERY,
             CONFIG_BENCH_PAYLOAD_BYTES,
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size(),
             current_rssi());

    print_resource_summary("before");

    int64_t run_start_us = esp_timer_get_time();
    for (int i = 0; i < CONFIG_BENCH_EVENT_COUNT; i++) {
        const honch_property_t properties[] = {
            honch_prop("run", honch_u64(run_id)),
            honch_prop("seq", honch_i64(i)),
            honch_prop("heap", honch_u64((uint32_t)esp_get_free_heap_size())),
            honch_prop("heap_min", honch_u64((uint32_t)esp_get_minimum_free_heap_size())),
            honch_prop("rssi", honch_i64(current_rssi())),
            honch_prop("pad", honch_str(pad))
        };

        int64_t start_us = esp_timer_get_time();
        honch_err_t track_err = honch_track("bench_event", properties, 6u);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        timer_add(&track_timer, elapsed_us, track_err == HONCH_OK);
        if (track_err == HONCH_OK) {
            s_queued_estimate++;
        }

        if (((i + 1) % CONFIG_BENCH_FLUSH_EVERY) == 0) {
            start_us = esp_timer_get_time();
            honch_err_t flush_err = honch_flush();
            elapsed_us = esp_timer_get_time() - start_us;
            timer_add(&flush_timer, elapsed_us, flush_err == HONCH_OK);
            if (flush_err == HONCH_OK) {
                s_queued_estimate = 0;
            }

            ESP_LOGI(TAG,
                     "BENCH_FLUSH_MARK run=%" PRIu32 " seq=%d notify_us=%" PRId64
                     " queued_estimate=%" PRIu32 " heap_free=%" PRIu32,
                     run_id,
                     i + 1,
                     elapsed_us,
                     s_queued_estimate,
                     (uint32_t)esp_get_free_heap_size());
        }

        if (CONFIG_BENCH_EVENT_INTERVAL_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_EVENT_INTERVAL_MS));
        }
    }

    int64_t final_flush_start_us = esp_timer_get_time();
    honch_err_t final_flush_err = honch_flush();
    int64_t final_flush_us = esp_timer_get_time() - final_flush_start_us;
    timer_add(&flush_timer, final_flush_us, final_flush_err == HONCH_OK);
    if (final_flush_err == HONCH_OK) {
        s_queued_estimate = 0;
    }

    ESP_LOGI(TAG,
             "BENCH_TRACK_SUMMARY run=%" PRIu32 " count=%" PRIu32 " failures=%" PRIu32
             " avg_us=%" PRId64 " min_us=%" PRId64 " max_us=%" PRId64,
             run_id,
             track_timer.count,
             track_timer.failures,
             timer_avg_us(&track_timer),
             track_timer.min_us,
             track_timer.max_us);

    ESP_LOGI(TAG,
             "BENCH_FLUSH_SUMMARY run=%" PRIu32 " notify_count=%" PRIu32
             " failures=%" PRIu32 " notify_avg_us=%" PRId64
             " notify_min_us=%" PRId64 " notify_max_us=%" PRId64,
             run_id,
             flush_timer.count,
             flush_timer.failures,
             timer_avg_us(&flush_timer),
             flush_timer.min_us,
             flush_timer.max_us);

    if (CONFIG_BENCH_SETTLE_SECONDS > 0) {
        ESP_LOGI(TAG, "BENCH_SETTLE seconds=%d", CONFIG_BENCH_SETTLE_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_SETTLE_SECONDS * 1000));
    }

    print_resource_summary("after");

    ESP_LOGI(TAG,
             "BENCH_RUN_END run=%" PRIu32 " elapsed_ms=%" PRId64
             " heap_free=%" PRIu32 " heap_min=%" PRIu32 " queued_estimate=%" PRIu32,
             run_id,
             (esp_timer_get_time() - run_start_us) / 1000,
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size(),
             s_queued_estimate);
}

static void run_offline_queue_proof(void)
{
    char pad[CONFIG_BENCH_PAYLOAD_BYTES + 1];
    uint32_t proof_id = (uint32_t)(esp_timer_get_time() / 1000);

    fill_pad(pad, sizeof(pad));

    ESP_LOGI(TAG,
             "BENCH_OFFLINE_PROOF_START proof_id=%" PRIu32
             " events=%d retry_seconds=%d queued_estimate=%" PRIu32,
             proof_id,
             CONFIG_BENCH_OFFLINE_QUEUE_PROOF_EVENTS,
             CONFIG_BENCH_OFFLINE_RECOVERY_RETRY_SECONDS,
             s_queued_estimate);
    ESP_LOGW(TAG,
             "BENCH_OFFLINE_RECOVERY_WAIT proof_id=%" PRIu32
             " action=keep_capture_down_until_queue_growth_then_restore",
             proof_id);

    for (int i = 0; i < CONFIG_BENCH_OFFLINE_QUEUE_PROOF_EVENTS; i++) {
        const honch_property_t properties[] = {
            honch_prop("proof_id", honch_u64(proof_id)),
            honch_prop("seq", honch_i64(i)),
            honch_prop("phase", honch_str("offline")),
            honch_prop("heap", honch_u64((uint32_t)esp_get_free_heap_size())),
            honch_prop("rssi", honch_i64(current_rssi())),
            honch_prop("pad", honch_str(pad))
        };

        honch_err_t track_err = honch_track("bench_offline_queue_proof", properties, 6u);
        if (track_err == HONCH_OK) {
            s_queued_estimate++;
        } else {
            ESP_LOGE(TAG,
                     "BENCH_OFFLINE_TRACK_FAILURE proof_id=%" PRIu32
                     " seq=%d err=%d queued_estimate=%" PRIu32,
                     proof_id,
                     i,
                     track_err,
                     s_queued_estimate);
        }

        if (CONFIG_BENCH_EVENT_INTERVAL_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_EVENT_INTERVAL_MS));
        }
    }

    ESP_LOGI(TAG,
             "BENCH_OFFLINE_QUEUE_GROWTH proof_id=%" PRIu32
             " queued_estimate=%" PRIu32 " expected_nonzero=%s",
             proof_id,
             s_queued_estimate,
             s_queued_estimate > 0 ? "yes" : "no");

    if (s_queued_estimate == 0) {
        ESP_LOGE(TAG, "BENCH_OFFLINE_PROOF_ABORT proof_id=%" PRIu32 " reason=no_queued_events", proof_id);
        return;
    }

    for (uint32_t attempt = 1;; attempt++) {
        int64_t start_us = esp_timer_get_time();
        honch_err_t flush_err = honch_flush();
        int64_t flush_us = esp_timer_get_time() - start_us;
        if (flush_err == HONCH_OK) {
            s_queued_estimate = 0;
            ESP_LOGI(TAG,
                     "BENCH_OFFLINE_RECOVERY_FLUSH proof_id=%" PRIu32
                     " attempt=%" PRIu32 " err=%d flush_us=%" PRId64
                     " queued_estimate=%" PRIu32,
                     proof_id,
                     attempt,
                     flush_err,
                     flush_us,
                     s_queued_estimate);
            break;
        }

        ESP_LOGW(TAG,
                 "BENCH_OFFLINE_RECOVERY_WAIT proof_id=%" PRIu32
                 " attempt=%" PRIu32 " err=%d flush_us=%" PRId64
                 " queued_estimate=%" PRIu32 " restore_capture_then_wait_seconds=%d",
                 proof_id,
                 attempt,
                 flush_err,
                 flush_us,
                 s_queued_estimate,
                 CONFIG_BENCH_OFFLINE_RECOVERY_RETRY_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_OFFLINE_RECOVERY_RETRY_SECONDS * 1000));
    }

    ESP_LOGI(TAG,
             "BENCH_OFFLINE_PROOF_DONE proof_id=%" PRIu32
             " queued_estimate=%" PRIu32 " clickhouse_event=bench_offline_queue_proof",
             proof_id,
             s_queued_estimate);
}

void app_main(void)
{
    if (!wifi_init_sta()) {
        return;
    }
    if (!CONFIG_BENCH_OFFLINE_QUEUE_PROOF && !wait_for_network_ready()) {
        return;
    }

    honch_config_t config = {
        .api_key = CONFIG_HONCH_API_KEY,
        .host = CONFIG_HONCH_HOST,
        .device_model = "bench-board",
        .firmware_version = "benchtest-0.1.0",
        .event_buffer = s_event_buffer,
        .event_buffer_size = sizeof(s_event_buffer),
    };

    honch_err_t err = honch_init(&config);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Failed to initialize Honch: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Honch initialized, device_id=%s", honch_get_device_id());

    err = honch_track_gpio(GPIO_NUM_0, "bench_button", HONCH_GPIO_FALLING_EDGE);
    if (err != HONCH_OK) {
        ESP_LOGW(TAG, "Failed to register GPIO benchmark marker: %d", err);
    }

    if (CONFIG_BENCH_OFFLINE_QUEUE_PROOF) {
        run_offline_queue_proof();
        ESP_LOGI(TAG, "Offline queue proof complete. Verify bench_offline_queue_proof rows in ClickHouse.");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }

    uint32_t run_id = 1;
    do {
        run_benchmark_once(run_id++);
#ifdef CONFIG_BENCH_REPEAT
        if (CONFIG_BENCH_REPEAT) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    } while (CONFIG_BENCH_REPEAT);
#else
    } while (false);
#endif

    ESP_LOGI(TAG, "Benchmark complete. Set BENCH_REPEAT to run continuously.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
