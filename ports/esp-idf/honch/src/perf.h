// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include <stdint.h>

#ifdef CONFIG_HONCH_PERF_LOGGING
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HONCH_PERF_VAR(type, name, value) type name = (value)
#define HONCH_PERF_NOW_US() esp_timer_get_time()
#define HONCH_PERF_ELAPSED_US(start_us) (esp_timer_get_time() - (start_us))
#define HONCH_PERF_HEAP_FREE() ((uint32_t)esp_get_free_heap_size())
#define HONCH_PERF_HEAP_MIN() ((uint32_t)esp_get_minimum_free_heap_size())
#define HONCH_PERF_HEAP_LARGEST() ((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
#define HONCH_PERF_LOG(marker, format, ...) \
    ESP_LOGI("honch_perf", marker " " format, ##__VA_ARGS__)
#else
#define HONCH_PERF_VAR(type, name, value) type name __attribute__((unused)) = (value)
#define HONCH_PERF_NOW_US() 0
#define HONCH_PERF_ELAPSED_US(start_us) 0
#define HONCH_PERF_HEAP_FREE() 0
#define HONCH_PERF_HEAP_MIN() 0
#define HONCH_PERF_HEAP_LARGEST() 0
#define HONCH_PERF_LOG(marker, format, ...) do { } while (0)
#endif
