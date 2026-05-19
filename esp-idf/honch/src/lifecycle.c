// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "lifecycle.h"
#include "identity.h"
#include "scheduler.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_system.h"

static const char *TAG = "honch";

extern honch_err_t honch_track(const char *event, const char *properties_json);
extern volatile bool g_honch_connected;
extern int (*g_honch_battery_callback)(void);
extern int g_honch_battery_low_threshold;

static esp_event_handler_instance_t s_wifi_disconnect_handler = NULL;
static esp_event_handler_instance_t s_ip_got_handler = NULL;
static bool s_battery_low_emitted = false;

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        default:                return "unknown";
    }
}

static void wifi_disconnect_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    g_honch_connected = false;
    ESP_LOGI(TAG, "Wi-Fi disconnected");
    honch_track("$connectivity_change", "{\"state\":\"disconnected\"}");
}

static void ip_got_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    g_honch_connected = true;
    ESP_LOGI(TAG, "Got IP, connectivity restored");
    honch_track("$connectivity_change", "{\"state\":\"connected\"}");
    honch_scheduler_notify();
}

honch_err_t honch_lifecycle_init(void)
{
    s_battery_low_emitted = false;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        g_honch_connected = true;
        ESP_LOGI(TAG, "Initial Wi-Fi connection detected");
    }

    // Register Wi-Fi disconnect event
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        wifi_disconnect_handler, NULL, &s_wifi_disconnect_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register Wi-Fi disconnect handler: %s", esp_err_to_name(err));
    }

    // Register IP got event
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        ip_got_handler, NULL, &s_ip_got_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
    }

    return HONCH_OK;
}

void honch_lifecycle_deinit(void)
{
    if (s_wifi_disconnect_handler) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_disconnect_handler);
        s_wifi_disconnect_handler = NULL;
    }
    if (s_ip_got_handler) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_got_handler);
        s_ip_got_handler = NULL;
    }
}

honch_err_t honch_lifecycle_emit_boot(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char *reason_str = reset_reason_str(reason);

    char props[64];
    snprintf(props, sizeof(props), "{\"reset_reason\":\"%s\"}", reason_str);

    ESP_LOGI(TAG, "Device boot (reason: %s)", reason_str);
    return honch_track("$device_boot", props);
}

honch_err_t honch_lifecycle_emit_shutdown(void)
{
    ESP_LOGI(TAG, "Device shutdown");
    return honch_track("$device_shutdown", NULL);
}

honch_err_t honch_lifecycle_check_firmware(const char *current_version)
{
    char prev_version[64];
    bool changed = false;

    honch_err_t err = honch_identity_check_firmware(
        current_version, prev_version, sizeof(prev_version), &changed);
    if (err != HONCH_OK) {
        return err;
    }

    if (changed) {
        char props[160];
        snprintf(props, sizeof(props),
                 "{\"previous_version\":\"%s\",\"new_version\":\"%s\"}",
                 prev_version, current_version);
        ESP_LOGI(TAG, "Firmware updated: %s -> %s", prev_version, current_version);
        return honch_track("$firmware_update", props);
    }

    return HONCH_OK;
}

void honch_lifecycle_check_battery(int level)
{
    if (level < 0) {
        return;
    }

    if (level < g_honch_battery_low_threshold) {
        if (!s_battery_low_emitted) {
            s_battery_low_emitted = true;
            char props[32];
            snprintf(props, sizeof(props), "{\"level\":%d}", level);
            ESP_LOGW(TAG, "Battery low: %d%%", level);
            honch_track("$battery_low", props);
        }
    } else {
        // Level recovered above threshold
        s_battery_low_emitted = false;
    }
}
