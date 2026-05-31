// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#include "esp_gpio_adapter.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HONCH_ESP_GPIO_LOCK_TIMEOUT_MS 10u

static bool s_gpio_initialized = false;
static StaticSemaphore_t s_gpio_lifecycle_mutex_storage;
static SemaphoreHandle_t s_gpio_lifecycle_mutex = NULL;
static portMUX_TYPE s_gpio_lifecycle_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

extern bool honch_esp_is_initialized(void);

static TickType_t honch_gpio_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static honch_err_t honch_gpio_lifecycle_lock(void)
{
    if (s_gpio_lifecycle_mutex == NULL) {
        portENTER_CRITICAL(&s_gpio_lifecycle_mutex_init_lock);
        if (s_gpio_lifecycle_mutex == NULL) {
            s_gpio_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_gpio_lifecycle_mutex_storage);
        }
        portEXIT_CRITICAL(&s_gpio_lifecycle_mutex_init_lock);
    }
    if (s_gpio_lifecycle_mutex == NULL) {
        return HONCH_ERR_INTERNAL;
    }
    if (xSemaphoreTake(s_gpio_lifecycle_mutex, honch_gpio_ticks(HONCH_ESP_GPIO_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return HONCH_ERR_BUSY;
    }
    return HONCH_OK;
}

static void honch_gpio_lifecycle_unlock(void)
{
    if (s_gpio_lifecycle_mutex != NULL) {
        (void)xSemaphoreGive(s_gpio_lifecycle_mutex);
    }
}

void honch_esp_gpio_shutdown_hook(void)
{
    if (honch_gpio_lifecycle_lock() != HONCH_OK) {
        return;
    }

    if (!s_gpio_initialized) {
        honch_gpio_lifecycle_unlock();
        return;
    }

    honch_gpio_deinit();
    s_gpio_initialized = false;
    honch_gpio_lifecycle_unlock();
}

honch_err_t honch_track_gpio(gpio_num_t pin, const char *event_name, honch_gpio_mode_t mode)
{
    if (!honch_esp_is_initialized()) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    honch_err_t err = honch_gpio_lifecycle_lock();
    if (err != HONCH_OK) {
        return err;
    }

    bool initialized_by_call = false;
    if (!s_gpio_initialized) {
        err = honch_gpio_init();
        if (err != HONCH_OK) {
            honch_gpio_lifecycle_unlock();
            return err;
        }
        s_gpio_initialized = true;
        initialized_by_call = true;
    }

    err = honch_gpio_register(pin, event_name, mode);
    if (err != HONCH_OK && initialized_by_call) {
        honch_gpio_deinit();
        s_gpio_initialized = false;
    }
    honch_gpio_lifecycle_unlock();
    return err;
}
