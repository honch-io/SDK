// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch/core/platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct honch_esp_platform {
    SemaphoreHandle_t mutex;
} honch_esp_platform_t;

honch_status_t honch_esp_platform_ops_init(honch_platform_ops_t *ops, honch_esp_platform_t *ctx);
void honch_esp_platform_ops_deinit(honch_esp_platform_t *ctx);
