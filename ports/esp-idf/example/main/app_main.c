// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "honch.h"
#include "water_filter.h"

static const char *TAG = "honch_example";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5
#define HEARTBEAT_INTERVAL_SECONDS 5
#define HONCH_TELEMETRY_TASK_STACK_BYTES 8192
#define HONCH_IDENTIFY_PILOT_TASK_STACK_BYTES 16384
#define HONCH_TELEMETRY_TASK_PRIORITY 2
#define HONCH_TICK_INTERVAL_MS 1000

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static void init_network_stack(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

static void init_wifi_nvs(void)
{
    // ESP-IDF Wi-Fi opens NVS during esp_wifi_init(), even when the station
    // configuration is later stored in RAM only.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
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

static void wifi_init_sta(void)
{
    init_wifi_nvs();

    s_wifi_event_group = xEventGroupCreate();

    init_network_stack();
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

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to Wi-Fi SSID: %s", CONFIG_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
    }
}

static void sync_time(void)
{
    ESP_LOGI(TAG, "Synchronizing time with SNTP...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};
    for (int retry = 0; retry < 15; retry++) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "Time synchronized");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "Time sync timed out; events may use boot-relative timestamps");
}

static uint8_t s_event_buffer[16384];

static int run_identify_pilot(void)
{
    uint32_t run_nonce = esp_random();
    char run_id[32];
    char user_id[64];
    char pre_event[64];
    char post_event[64];

    snprintf(run_id, sizeof(run_id), "%08" PRIx32, run_nonce);
    snprintf(user_id, sizeof(user_id), "esp32-identify-user-%s", run_id);
    snprintf(pre_event, sizeof(pre_event), "esp32_identify_pre_%s", run_id);
    snprintf(post_event, sizeof(post_event), "esp32_identify_post_%s", run_id);

    const char *device_id = honch_get_device_id();
    ESP_LOGI(TAG, "IDENTIFY_PILOT run_id=%s device_id=%s user_id=%s", run_id, device_id, user_id);

    const honch_property_t pre_properties[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("phase", honch_str("pre_identify")),
    };
    honch_err_t pre_status = honch_track(pre_event, pre_properties, 2u);
    ESP_LOGI(TAG, "IDENTIFY_PILOT pre_event=%s status=%d", pre_event, pre_status);
    if (pre_status != HONCH_OK) {
        return 1;
    }

    const honch_property_t traits[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("pilot", honch_str("esp32-identify")),
    };
    honch_err_t identify_status = honch_identify(user_id, traits, 2u);
    ESP_LOGI(TAG, "IDENTIFY_PILOT identify_user_id=%s status=%d", user_id, identify_status);
    if (identify_status != HONCH_OK) {
        return 1;
    }

    const honch_property_t post_properties[] = {
        honch_prop("run_id", honch_str(run_id)),
        honch_prop("phase", honch_str("post_identify")),
    };
    honch_err_t post_status = honch_track(post_event, post_properties, 2u);
    ESP_LOGI(TAG, "IDENTIFY_PILOT post_event=%s status=%d", post_event, post_status);
    if (post_status != HONCH_OK) {
        return 1;
    }

    honch_err_t flush_status = honch_flush();
    ESP_LOGI(TAG, "IDENTIFY_PILOT flush status=%d", flush_status);
    ESP_LOGI(TAG, "IDENTIFY_PILOT complete run_id=%s ok=%s", run_id, flush_status == HONCH_OK ? "true" : "false");
    return flush_status == HONCH_OK ? 0 : 1;
}

static void honch_identify_pilot_task(void *arg)
{
    (void)arg;
    int pilot_status = run_identify_pilot();
    ESP_LOGI(TAG, "IDENTIFY_PILOT finished status=%d", pilot_status);
    vTaskDelete(NULL);
}

static void honch_telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        honch_err_t err = honch_tick();
        if (err != HONCH_OK && err != HONCH_ERR_TRANSPORT && err != HONCH_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "honch_tick: %d", err);
        }
        vTaskDelay(pdMS_TO_TICKS(HONCH_TICK_INTERVAL_MS));
    }
}

void app_main(void)
{
    bool offline_smoke = strlen(CONFIG_WIFI_SSID) == 0;
    if (!offline_smoke) {
        wifi_init_sta();
        sync_time();
    } else {
        init_network_stack();
        ESP_LOGW(TAG, "No Wi-Fi SSID configured; running offline tick smoke test");
    }

    // Initialize Honch
    honch_config_t config = {
        .api_key = strlen(CONFIG_HONCH_API_KEY) > 0 ? CONFIG_HONCH_API_KEY : "offline-test-key",
        .host = offline_smoke ? "http://127.0.0.1:9" : CONFIG_HONCH_HOST,
        .device_model = "demo-board",
        .firmware_version = "0.1.0",
        .event_buffer = s_event_buffer,
        .event_buffer_size = sizeof(s_event_buffer),
        .flush_event_threshold = 1,
#ifdef CONFIG_HONCH_IDENTIFY_PILOT
        .flush_max_batches = 8,
        .shutdown_flush_max_batches = 8,
#endif
    };

    honch_err_t err = honch_init(&config);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Failed to initialize Honch: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Honch initialized, device_id: %s", honch_get_device_id());

#ifdef CONFIG_HONCH_IDENTIFY_PILOT
    if (CONFIG_HONCH_IDENTIFY_PILOT) {
        BaseType_t pilot_task_created = xTaskCreate(honch_identify_pilot_task,
            "honch_identify_pilot",
            HONCH_IDENTIFY_PILOT_TASK_STACK_BYTES,
            NULL,
            HONCH_TELEMETRY_TASK_PRIORITY,
            NULL);
        if (pilot_task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to start Honch identify pilot task");
        }
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_SECONDS * 1000));
        }
    }
#endif

    water_filter_run_demo();

    BaseType_t task_created = xTaskCreate(honch_telemetry_task,
        "honch_telemetry",
        HONCH_TELEMETRY_TASK_STACK_BYTES,
        NULL,
        HONCH_TELEMETRY_TASK_PRIORITY,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start Honch telemetry task");
    }

    // Idle loop
    while (1) {
        water_filter_emit_health_report();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_SECONDS * 1000));
    }
}
