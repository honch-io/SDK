// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "water_filter.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "honch.h"

static const char *TAG = "water_filter";

typedef struct {
    int filter_life_percent;
    int tank_liters_x10;
    int dispense_count;
    int health_report_count;
} water_filter_state_t;

static water_filter_state_t s_filter = {
    .filter_life_percent = 87,
    .tank_liters_x10 = 126,
    .dispense_count = 1842,
    .health_report_count = 0,
};

static void log_honch_result(const char *operation, honch_err_t err)
{
    ESP_LOGI(TAG, "%s: %d", operation, err);
}

static int uptime_seconds(void)
{
    return (int)(xTaskGetTickCount() / configTICK_RATE_HZ);
}

void water_filter_run_demo(void)
{
    const honch_property_t operator_traits[] = {
        honch_prop("name", honch_str("Alex Rivera")),
        honch_prop("role", honch_str("facility_manager")),
        honch_prop("account_id", honch_str("acct_riverfront_amenities")),
        honch_prop("site_id", honch_str("site_lobby_north")),
        honch_prop("device_fleet", honch_str("filtered-water-stations")),
    };
    honch_err_t err = honch_identify("operator_alex_rivera", operator_traits, 5u);
    log_honch_result("honch_identify operator_alex_rivera", err);

    const honch_property_t provisioned[] = {
        honch_prop("site_id", honch_str("site_lobby_north")),
        honch_prop("asset_tag", honch_str("WF-NORTH-042")),
        honch_prop("install_floor", honch_i64(1)),
        honch_prop("commissioned", honch_bool(true)),
    };
    err = honch_track("device_provisioned", provisioned, 4u);
    log_honch_result("honch_track device_provisioned", err);

    err = honch_session_start("morning-maintenance");
    log_honch_result("honch_session_start morning-maintenance", err);

    const honch_property_t status_checked[] = {
        honch_prop("filter_life_percent", honch_i64(s_filter.filter_life_percent)),
        honch_prop("tank_liters_remaining", honch_f64((double)s_filter.tank_liters_x10 / 10.0)),
        honch_prop("water_temperature_c", honch_f64(6.8)),
        honch_prop("tds_ppm", honch_i64(142)),
        honch_prop("leak_detected", honch_bool(false)),
    };
    err = honch_track("filter_status_checked", status_checked, 5u);
    log_honch_result("honch_track filter_status_checked", err);

    const honch_property_t filter_replaced[] = {
        honch_prop("old_filter_life_percent", honch_i64(s_filter.filter_life_percent)),
        honch_prop("new_filter_life_percent", honch_i64(100)),
        honch_prop("replacement_reason", honch_str("scheduled_maintenance")),
        honch_prop("cartridge_sku", honch_str("HF-2000")),
    };
    err = honch_track("filter_replaced", filter_replaced, 4u);
    log_honch_result("honch_track filter_replaced", err);
    s_filter.filter_life_percent = 100;

    err = honch_session_end();
    log_honch_result("honch_session_end morning-maintenance", err);

    err = honch_session_start("guest-dispense");
    log_honch_result("honch_session_start guest-dispense", err);

    const honch_property_t dispense_started[] = {
        honch_prop("button", honch_str("chilled")),
        honch_prop("requested_volume_ml", honch_i64(500)),
        honch_prop("filter_life_percent", honch_i64(s_filter.filter_life_percent)),
        honch_prop("tank_liters_remaining", honch_f64((double)s_filter.tank_liters_x10 / 10.0)),
    };
    err = honch_track("dispense_started", dispense_started, 4u);
    log_honch_result("honch_track dispense_started", err);

    s_filter.dispense_count++;
    s_filter.tank_liters_x10 -= 5;

    const honch_property_t dispense_completed[] = {
        honch_prop("dispense_count", honch_i64(s_filter.dispense_count)),
        honch_prop("actual_volume_ml", honch_i64(492)),
        honch_prop("duration_ms", honch_i64(4200)),
        honch_prop("flow_rate_ml_s", honch_f64(117.1)),
        honch_prop("water_temperature_c", honch_f64(6.9)),
    };
    err = honch_track("dispense_completed", dispense_completed, 5u);
    log_honch_result("honch_track dispense_completed", err);

    err = honch_session_end();
    log_honch_result("honch_session_end guest-dispense", err);

    err = honch_session_start("daily-operation");
    log_honch_result("honch_session_start daily-operation", err);
}

void water_filter_emit_health_report(void)
{
    s_filter.health_report_count++;
    if (s_filter.health_report_count % 4 == 0 && s_filter.filter_life_percent > 0) {
        s_filter.filter_life_percent--;
    }

    const honch_property_t health[] = {
        honch_prop("uptime_seconds", honch_i64(uptime_seconds())),
        honch_prop("filter_life_percent", honch_i64(s_filter.filter_life_percent)),
        honch_prop("tank_liters_remaining", honch_f64((double)s_filter.tank_liters_x10 / 10.0)),
        honch_prop("dispense_count", honch_i64(s_filter.dispense_count)),
        honch_prop("water_temperature_c", honch_f64(7.1)),
        honch_prop("tds_ppm", honch_i64(145)),
        honch_prop("free_heap_bytes", honch_i64((int64_t)esp_get_free_heap_size())),
        honch_prop("service_required", honch_bool(s_filter.filter_life_percent < 15)),
    };
    honch_err_t err = honch_track("device_health_report", health, 8u);
    log_honch_result("honch_track device_health_report", err);
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());
}
