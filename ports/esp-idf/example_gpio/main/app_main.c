// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "honch.h"
#include "nvs_flash.h"

static const char *TAG = "honch_gpio_example";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 5
#define HONCH_TELEMETRY_TASK_STACK_WORDS 4096
#define APP_GPIO_TASK_STACK_WORDS 4096
#define HONCH_TASK_PRIORITY 2
#define HONCH_TICK_INTERVAL_MS 1000
#define GPIO_DEBOUNCE_MS 50

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static uint8_t s_event_buffer[16384];
static QueueHandle_t s_gpio_queue;

static void init_wifi_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
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
    init_wifi_nvs();

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

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to Wi-Fi SSID: %s", CONFIG_WIFI_SSID);
        return true;
    }

    ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
    return false;
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

static void IRAM_ATTR button_isr_handler(void *arg)
{
    QueueHandle_t queue = s_gpio_queue;
    if (queue != NULL) {
        uint32_t pin = (uint32_t)(uintptr_t)arg;
        (void)xQueueSendFromISR(queue, &pin, NULL);
    }
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

static void gpio_tracking_task(void *arg)
{
    (void)arg;
    int64_t last_trigger_us = 0;
    uint32_t pin = 0;

    for (;;) {
        if (xQueueReceive(s_gpio_queue, &pin, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_trigger_us) < (GPIO_DEBOUNCE_MS * 1000)) {
            continue;
        }
        last_trigger_us = now_us;

        const honch_property_t props[] = {
            honch_prop("pin", honch_i64((int64_t)pin)),
        };
        honch_err_t err = honch_track("button_pressed", props, 1u);
        if (err != HONCH_OK) {
            ESP_LOGW(TAG, "honch_track button_pressed: %d", err);
        }
    }
}

static esp_err_t init_button_gpio(void)
{
    gpio_num_t button = (gpio_num_t)CONFIG_HONCH_GPIO_BUTTON;
    if (button < 0 || button >= GPIO_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_queue = xQueueCreate(16, sizeof(uint32_t));
    if (s_gpio_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << button),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return err;
    }

    err = gpio_isr_handler_add(button, button_isr_handler, (void *)(uintptr_t)button);
    if (err != ESP_OK) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return err;
    }

    BaseType_t task_created = xTaskCreate(
        gpio_tracking_task,
        "app_gpio",
        APP_GPIO_TASK_STACK_WORDS,
        NULL,
        HONCH_TASK_PRIORITY,
        NULL);
    if (task_created != pdPASS) {
        gpio_isr_handler_remove(button);
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void)
{
    if (!wifi_init_sta()) {
        return;
    }
    sync_time();

    honch_config_t config = {
        .api_key = CONFIG_HONCH_API_KEY,
        .host = CONFIG_HONCH_HOST,
        .device_model = "demo-board",
        .firmware_version = "0.1.0",
        .event_buffer = s_event_buffer,
        .event_buffer_size = sizeof(s_event_buffer),
        .flush_event_threshold = 1,
    };

    honch_err_t err = honch_init(&config);
    if (err != HONCH_OK) {
        ESP_LOGE(TAG, "Failed to initialize Honch: %d", err);
        return;
    }

    BaseType_t task_created = xTaskCreate(
        honch_telemetry_task,
        "honch_telemetry",
        HONCH_TELEMETRY_TASK_STACK_WORDS,
        NULL,
        HONCH_TASK_PRIORITY,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start Honch telemetry task");
        return;
    }

    esp_err_t gpio_err = init_button_gpio();
    if (gpio_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize host-owned GPIO tracking: %s", esp_err_to_name(gpio_err));
        return;
    }

    ESP_LOGI(TAG, "Press GPIO %d to send button_pressed events", CONFIG_HONCH_GPIO_BUTTON);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
