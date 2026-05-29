// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#include "esp_gpio_adapter.h"

#include <pthread.h>
#include <stdbool.h>

static bool s_gpio_initialized = false;
static pthread_mutex_t s_gpio_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

extern bool honch_esp_is_initialized(void);

static honch_err_t honch_gpio_lifecycle_lock(void)
{
    return pthread_mutex_lock(&s_gpio_lifecycle_mutex) == 0 ? HONCH_OK : HONCH_ERR_INTERNAL;
}

static void honch_gpio_lifecycle_unlock(void)
{
    (void)pthread_mutex_unlock(&s_gpio_lifecycle_mutex);
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
