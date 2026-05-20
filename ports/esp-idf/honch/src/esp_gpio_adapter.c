// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_gpio_adapter.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// Forward declaration — implemented in honch.c
extern honch_err_t honch_track(const char *event, const char *properties_json);

static const char *TAG = "honch";

#define MAX_GPIO_PINS 8
#define DEBOUNCE_MS 50

typedef struct {
    gpio_num_t pin;
    char event_name[64];
    int64_t last_trigger_us;
} gpio_mapping_t;

static gpio_mapping_t s_mappings[MAX_GPIO_PINS];
static int s_mapping_count = 0;
static QueueHandle_t s_gpio_queue = NULL;
static TaskHandle_t s_gpio_task = NULL;
static volatile bool s_running = false;
static bool s_isr_service_installed = false;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(s_gpio_queue, &pin, NULL);
}

static void gpio_worker_task(void *arg)
{
    (void)arg;
    uint32_t pin;

    while (s_running) {
        if (xQueueReceive(s_gpio_queue, &pin, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Find the mapping
            for (int i = 0; i < s_mapping_count; i++) {
                if (s_mappings[i].pin == (gpio_num_t)pin) {
                    // Debounce check
                    int64_t now = esp_timer_get_time();
                    if ((now - s_mappings[i].last_trigger_us) < (DEBOUNCE_MS * 1000)) {
                        break;
                    }
                    s_mappings[i].last_trigger_us = now;

                    // Track the event
                    char props[32];
                    snprintf(props, sizeof(props), "{\"pin\":%d}", (int)pin);
                    honch_track(s_mappings[i].event_name, props);
                    break;
                }
            }
        }
    }

    vTaskDelete(NULL);
}

honch_err_t honch_gpio_init(void)
{
    s_mapping_count = 0;
    memset(s_mappings, 0, sizeof(s_mappings));

    s_gpio_queue = xQueueCreate(16, sizeof(uint32_t));
    if (!s_gpio_queue) {
        return HONCH_ERR_NO_MEM;
    }

    s_running = true;

    BaseType_t ret = xTaskCreate(gpio_worker_task, "honch_gpio", 4096, NULL, 4, &s_gpio_task);
    if (ret != pdPASS) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return HONCH_ERR_NO_MEM;
    }

    return HONCH_OK;
}

void honch_gpio_deinit(void)
{
    s_running = false;

    // Remove ISR handlers
    for (int i = 0; i < s_mapping_count; i++) {
        gpio_isr_handler_remove(s_mappings[i].pin);
    }

    if (s_gpio_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_gpio_task = NULL;
    }

    if (s_gpio_queue) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
    }

    s_mapping_count = 0;
}

honch_err_t honch_gpio_register(gpio_num_t pin, const char *event_name,
                                 honch_gpio_mode_t mode)
{
    if (!event_name) {
        return HONCH_ERR_INVALID_ARG;
    }

    // Check if pin is already registered, replace if so
    int idx = -1;
    for (int i = 0; i < s_mapping_count; i++) {
        if (s_mappings[i].pin == pin) {
            idx = i;
            gpio_isr_handler_remove(pin);
            break;
        }
    }

    if (idx < 0) {
        if (s_mapping_count >= MAX_GPIO_PINS) {
            ESP_LOGE(TAG, "Maximum GPIO pins (%d) already registered", MAX_GPIO_PINS);
            return HONCH_ERR_NO_MEM;
        }
        idx = s_mapping_count++;
    }

    s_mappings[idx].pin = pin;
    strncpy(s_mappings[idx].event_name, event_name, sizeof(s_mappings[idx].event_name) - 1);
    s_mappings[idx].event_name[sizeof(s_mappings[idx].event_name) - 1] = '\0';
    s_mappings[idx].last_trigger_us = 0;

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    switch (mode) {
        case HONCH_GPIO_RISING_EDGE:
            io_conf.intr_type = GPIO_INTR_POSEDGE;
            break;
        case HONCH_GPIO_FALLING_EDGE:
            io_conf.intr_type = GPIO_INTR_NEGEDGE;
            break;
        case HONCH_GPIO_BOTH_EDGES:
            io_conf.intr_type = GPIO_INTR_ANYEDGE;
            break;
    }

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", pin, esp_err_to_name(err));
        s_mapping_count--;
        return HONCH_ERR_INTERNAL;
    }

    // Install ISR service if not already done
    if (!s_isr_service_installed) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            // ESP_ERR_INVALID_STATE means already installed, which is fine
            s_isr_service_installed = true;
        } else {
            ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
            return HONCH_ERR_INTERNAL;
        }
    }

    err = gpio_isr_handler_add(pin, gpio_isr_handler, (void *)(uintptr_t)pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %s", pin, esp_err_to_name(err));
        return HONCH_ERR_INTERNAL;
    }

    ESP_LOGI(TAG, "GPIO %d registered for event '%s'", pin, event_name);
    return HONCH_OK;
}
