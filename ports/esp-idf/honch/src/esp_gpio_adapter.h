// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"
#include "driver/gpio.h"

honch_err_t honch_gpio_init(void);
void honch_gpio_deinit(void);

honch_err_t honch_gpio_register(gpio_num_t pin, const char *event_name,
                                 honch_gpio_mode_t mode);
