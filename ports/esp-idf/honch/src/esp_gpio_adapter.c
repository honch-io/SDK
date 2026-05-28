// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_gpio_adapter.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// Forward declaration — implemented in honch.c
extern honch_err_t honch_track(const char *event, const char *properties_json);

static const char *TAG = "honch";

#define MAX_GPIO_PINS 8
#define DEBOUNCE_MS 50
#define HONCH_GPIO_SHUTDOWN_SENTINEL UINT32_MAX

typedef struct {
    gpio_num_t pin;
    char event_name[64];
    int64_t last_trigger_us;
} gpio_mapping_t;

static gpio_mapping_t s_mappings[MAX_GPIO_PINS];
static int s_mapping_count = 0;
static QueueHandle_t s_gpio_queue = NULL;
static TaskHandle_t s_gpio_task = NULL;
static SemaphoreHandle_t s_gpio_exit_sem = NULL;
static volatile bool s_running = false;
static bool s_isr_service_installed = false;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    QueueHandle_t queue = s_gpio_queue;
    if (queue != NULL) {
        uint32_t pin = (uint32_t)(uintptr_t)arg;
        xQueueSendFromISR(queue, &pin, NULL);
    }
}

static void honch_gpio_rollback_mapping(bool added_mapping, int idx)
{
    if (!added_mapping || idx < 0 || idx >= s_mapping_count || idx >= MAX_GPIO_PINS) {
        return;
    }

    memset(&s_mappings[idx], 0, sizeof(s_mappings[idx]));
    if (idx == s_mapping_count - 1) {
        s_mapping_count -= 1;
    }
}

static void gpio_worker_task(void *arg)
{
    (void)arg;
    uint32_t pin;

    while (true) {
        if (xQueueReceive(s_gpio_queue, &pin, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (!s_running || pin == HONCH_GPIO_SHUTDOWN_SENTINEL) {
                break;
            }

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

    if (s_gpio_exit_sem != NULL) {
        xSemaphoreGive(s_gpio_exit_sem);
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

    s_gpio_exit_sem = xSemaphoreCreateBinary();
    if (!s_gpio_exit_sem) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
        return HONCH_ERR_NO_MEM;
    }

    s_running = true;

    BaseType_t ret = xTaskCreate(gpio_worker_task, "honch_gpio", 4096, NULL, 4, &s_gpio_task);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_gpio_exit_sem);
        s_gpio_exit_sem = NULL;
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

    TaskHandle_t gpio_task = s_gpio_task;
    if (gpio_task && s_gpio_queue) {
        uint32_t sentinel = HONCH_GPIO_SHUTDOWN_SENTINEL;
        (void)xQueueSend(s_gpio_queue, &sentinel, 0);
    }

    if (gpio_task && s_gpio_exit_sem) {
        (void)xSemaphoreTake(s_gpio_exit_sem, portMAX_DELAY);
        s_gpio_task = NULL;
    }

    if (s_gpio_queue) {
        vQueueDelete(s_gpio_queue);
        s_gpio_queue = NULL;
    }

    if (s_gpio_exit_sem) {
        vSemaphoreDelete(s_gpio_exit_sem);
        s_gpio_exit_sem = NULL;
    }

    s_mapping_count = 0;
}

honch_err_t honch_gpio_register(gpio_num_t pin, const char *event_name,
                                 honch_gpio_mode_t mode)
{
    if (!event_name) {
        return HONCH_ERR_INVALID_ARG;
    }
    if (pin < 0 || pin >= GPIO_NUM_MAX || pin >= 64) {
        return HONCH_ERR_INVALID_ARG;
    }

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
        default:
            return HONCH_ERR_INVALID_ARG;
    }

    gpio_mapping_t next_mapping = {
        .pin = pin,
        .last_trigger_us = 0
    };
    strncpy(next_mapping.event_name, event_name, sizeof(next_mapping.event_name) - 1);
    next_mapping.event_name[sizeof(next_mapping.event_name) - 1] = '\0';

    // Check if pin is already registered, replace if so
    int idx = -1;
    for (int i = 0; i < s_mapping_count; i++) {
        if (s_mappings[i].pin == pin) {
            idx = i;
            break;
        }
    }

    bool added_mapping = false;
    if (idx < 0) {
        if (s_mapping_count >= MAX_GPIO_PINS) {
            ESP_LOGE(TAG, "Maximum GPIO pins (%d) already registered", MAX_GPIO_PINS);
            return HONCH_ERR_NO_MEM;
        }
        idx = s_mapping_count;
        added_mapping = true;
    }

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", pin, esp_err_to_name(err));
        honch_gpio_rollback_mapping(added_mapping, idx);
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
            honch_gpio_rollback_mapping(added_mapping, idx);
            return HONCH_ERR_INTERNAL;
        }
    }

    if (!added_mapping) {
        gpio_isr_handler_remove(pin);
    }
    err = gpio_isr_handler_add(pin, gpio_isr_handler, (void *)(uintptr_t)pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %s", pin, esp_err_to_name(err));
        honch_gpio_rollback_mapping(added_mapping, idx);
        return HONCH_ERR_INTERNAL;
    }

    s_mappings[idx].pin = next_mapping.pin;
    strncpy(s_mappings[idx].event_name, next_mapping.event_name, sizeof(s_mappings[idx].event_name) - 1);
    s_mappings[idx].event_name[sizeof(s_mappings[idx].event_name) - 1] = '\0';
    s_mappings[idx].last_trigger_us = next_mapping.last_trigger_us;
    if (added_mapping) {
        s_mapping_count++;
    }
    ESP_LOGI(TAG, "GPIO %d registered for event '%s'", pin, event_name);
    return HONCH_OK;
}
