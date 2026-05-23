// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "honch.h"

#include "esp_gpio_adapter.h"

#include <stdbool.h>

static bool s_gpio_initialized = false;

extern bool honch_esp_is_initialized(void);

void honch_esp_gpio_shutdown_hook(void)
{
    if (!s_gpio_initialized) {
        return;
    }

    honch_gpio_deinit();
    s_gpio_initialized = false;
}

honch_err_t honch_track_gpio(gpio_num_t pin, const char *event_name, honch_gpio_mode_t mode)
{
    if (!honch_esp_is_initialized()) {
        return HONCH_ERR_NOT_INITIALIZED;
    }

    bool initialized_by_call = false;
    if (!s_gpio_initialized) {
        honch_err_t err = honch_gpio_init();
        if (err != HONCH_OK) {
            return err;
        }
        s_gpio_initialized = true;
        initialized_by_call = true;
    }

    honch_err_t err = honch_gpio_register(pin, event_name, mode);
    if (err != HONCH_OK && initialized_by_call) {
        honch_gpio_deinit();
        s_gpio_initialized = false;
    }
    return err;
}
