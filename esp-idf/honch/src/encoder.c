// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "encoder.h"
#include "identity.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"

static const char *TAG = "honch";

// These are set by honch.c at init time
extern const char *g_honch_device_model;
extern const char *g_honch_firmware_version;
extern const char *g_honch_environment;
extern int (*g_honch_battery_callback)(void);

static void get_iso8601_timestamp(char *buf, size_t buf_size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm timeinfo;
    gmtime_r(&tv.tv_sec, &timeinfo);
    int ms = tv.tv_usec / 1000;
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms);
}

char *honch_encode_event(const char *event_name,
                         const char *distinct_id,
                         const char *properties_json)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "event", event_name);
    cJSON_AddStringToObject(root, "distinct_id", distinct_id);

    char timestamp[32];
    get_iso8601_timestamp(timestamp, sizeof(timestamp));
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    // Build properties
    cJSON *props = cJSON_CreateObject();
    if (!props) {
        cJSON_Delete(root);
        return NULL;
    }

    // Merge user-supplied properties first (auto-stamped win on conflict)
    if (properties_json && strlen(properties_json) > 0) {
        cJSON *user_props = cJSON_Parse(properties_json);
        if (user_props) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, user_props) {
                // Add each property; duplicates will be overwritten by auto-stamp below
                cJSON *copy = cJSON_Duplicate(item, true);
                if (copy) {
                    // Delete existing key if present (auto-stamp will re-add)
                    cJSON_DeleteItemFromObject(props, item->string);
                    cJSON_AddItemToObject(props, item->string, copy);
                }
            }
            cJSON_Delete(user_props);
        } else {
            ESP_LOGW(TAG, "Failed to parse properties_json, ignoring user properties");
        }
    }

    // Auto-stamp properties (these win on conflict)
    const char *device_id = honch_identity_get_device_id();
    cJSON_DeleteItemFromObject(props, "$device_id");
    cJSON_AddStringToObject(props, "$device_id", device_id);

    cJSON_DeleteItemFromObject(props, "$device_model");
    cJSON_AddStringToObject(props, "$device_model", g_honch_device_model);

    cJSON_DeleteItemFromObject(props, "$firmware_version");
    cJSON_AddStringToObject(props, "$firmware_version", g_honch_firmware_version);

    cJSON_DeleteItemFromObject(props, "$sdk_platform");
    cJSON_AddStringToObject(props, "$sdk_platform", "esp-idf");

    cJSON_DeleteItemFromObject(props, "$sdk_version");
    cJSON_AddStringToObject(props, "$sdk_version", "0.1.0");

    cJSON_DeleteItemFromObject(props, "$environment");
    cJSON_AddStringToObject(props, "$environment", g_honch_environment);

    // Session ID (only if active)
    const char *session_id = honch_identity_get_session_id();
    if (session_id) {
        cJSON_DeleteItemFromObject(props, "$session_id");
        cJSON_AddStringToObject(props, "$session_id", session_id);
    }

    // Battery level (only if callback set and returns >= 0)
    if (g_honch_battery_callback) {
        int level = g_honch_battery_callback();
        if (level >= 0) {
            cJSON_DeleteItemFromObject(props, "$battery_level");
            cJSON_AddNumberToObject(props, "$battery_level", level);
        }
    }

    // Wi-Fi RSSI (only if connected)
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_DeleteItemFromObject(props, "$wifi_rssi");
        cJSON_AddNumberToObject(props, "$wifi_rssi", ap_info.rssi);
    }

    cJSON_AddItemToObject(root, "properties", props);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

#ifdef CONFIG_HONCH_LOG_VERBOSE
    if (json_str) {
        ESP_LOGI(TAG, "Encoded event: %s", json_str);
    }
#endif

    return json_str;
}

char *honch_encode_batch(const char *api_key, char **events, int count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "token", api_key);

    cJSON *batch = cJSON_CreateArray();
    if (!batch) {
        cJSON_Delete(root);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        cJSON *event = cJSON_Parse(events[i]);
        if (event) {
            cJSON_AddItemToArray(batch, event);
        } else {
            ESP_LOGW(TAG, "Failed to parse event %d for batch, skipping", i);
        }
    }

    cJSON_AddItemToObject(root, "batch", batch);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}
