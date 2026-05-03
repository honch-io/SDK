// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "identity.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "honch";
static const char *NVS_NAMESPACE = "honch";
static const char *NVS_KEY_DEVICE_ID = "device_id";
static const char *NVS_KEY_DISTINCT_ID = "distinct_id";

static char s_device_id[HONCH_DEVICE_ID_LEN];
static char s_distinct_id[128];
static char s_session_id[HONCH_SESSION_ID_LEN];
static bool s_session_active = false;

static void generate_device_id_from_mac(char *out)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(out, HONCH_DEVICE_ID_LEN, "dev_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void generate_uuid_v4(char *out, size_t out_size)
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    // Set version 4
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant 1
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    snprintf(out, out_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

honch_err_t honch_identity_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return HONCH_ERR_NVS;
    }

    // Load or generate device_id
    size_t len = sizeof(s_device_id);
    err = nvs_get_str(handle, NVS_KEY_DEVICE_ID, s_device_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        generate_device_id_from_mac(s_device_id);
        err = nvs_set_str(handle, NVS_KEY_DEVICE_ID, s_device_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write device_id to NVS: %s", esp_err_to_name(err));
            nvs_close(handle);
            return HONCH_ERR_NVS;
        }
        nvs_commit(handle);
        ESP_LOGI(TAG, "Generated device_id: %s", s_device_id);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device_id from NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return HONCH_ERR_NVS;
    } else {
        ESP_LOGI(TAG, "Loaded device_id: %s", s_device_id);
    }

    // Load distinct_id (defaults to device_id)
    len = sizeof(s_distinct_id);
    err = nvs_get_str(handle, NVS_KEY_DISTINCT_ID, s_distinct_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(s_distinct_id, s_device_id, sizeof(s_distinct_id) - 1);
        s_distinct_id[sizeof(s_distinct_id) - 1] = '\0';
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read distinct_id, using device_id");
        strncpy(s_distinct_id, s_device_id, sizeof(s_distinct_id) - 1);
        s_distinct_id[sizeof(s_distinct_id) - 1] = '\0';
    }

    s_session_active = false;
    s_session_id[0] = '\0';

    nvs_close(handle);
    return HONCH_OK;
}

void honch_identity_deinit(void)
{
    s_session_active = false;
    s_session_id[0] = '\0';
}

const char *honch_identity_get_device_id(void)
{
    return s_device_id;
}

const char *honch_identity_get_distinct_id(void)
{
    return s_distinct_id;
}

honch_err_t honch_identity_set_distinct_id(const char *distinct_id)
{
    if (!distinct_id || strlen(distinct_id) == 0) {
        return HONCH_ERR_INVALID_ARG;
    }

    strncpy(s_distinct_id, distinct_id, sizeof(s_distinct_id) - 1);
    s_distinct_id[sizeof(s_distinct_id) - 1] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return HONCH_ERR_NVS;
    }
    err = nvs_set_str(handle, NVS_KEY_DISTINCT_ID, s_distinct_id);
    if (err != ESP_OK) {
        nvs_close(handle);
        return HONCH_ERR_NVS;
    }
    nvs_commit(handle);
    nvs_close(handle);
    return HONCH_OK;
}

const char *honch_identity_get_session_id(void)
{
    if (!s_session_active) {
        return NULL;
    }
    return s_session_id;
}

honch_err_t honch_identity_start_session(void)
{
    char uuid[37];
    generate_uuid_v4(uuid, sizeof(uuid));
    snprintf(s_session_id, sizeof(s_session_id), "sess_%s", uuid);
    s_session_active = true;
    return HONCH_OK;
}

void honch_identity_end_session(void)
{
    s_session_active = false;
    s_session_id[0] = '\0';
}

honch_err_t honch_identity_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return HONCH_ERR_NVS;
    }

    nvs_erase_key(handle, NVS_KEY_DEVICE_ID);
    nvs_erase_key(handle, NVS_KEY_DISTINCT_ID);
    nvs_commit(handle);
    nvs_close(handle);

    // Regenerate device_id
    generate_device_id_from_mac(s_device_id);

    // Persist new device_id
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_DEVICE_ID, s_device_id);
        nvs_commit(handle);
        nvs_close(handle);
    }

    // Reset distinct_id to new device_id
    strncpy(s_distinct_id, s_device_id, sizeof(s_distinct_id) - 1);
    s_distinct_id[sizeof(s_distinct_id) - 1] = '\0';

    // Clear session
    s_session_active = false;
    s_session_id[0] = '\0';

    ESP_LOGI(TAG, "Identity reset, new device_id: %s", s_device_id);
    return HONCH_OK;
}

honch_err_t honch_identity_check_firmware(const char *current_version,
                                           char *prev_version_out,
                                           size_t prev_version_size,
                                           bool *changed)
{
    *changed = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return HONCH_ERR_NVS;
    }

    char stored[64] = {0};
    size_t len = sizeof(stored);
    err = nvs_get_str(handle, "last_fw_ver", stored, &len);

    if (err == ESP_OK && strlen(stored) > 0 && strcmp(stored, current_version) != 0) {
        *changed = true;
        strncpy(prev_version_out, stored, prev_version_size - 1);
        prev_version_out[prev_version_size - 1] = '\0';
    }

    // Always update stored version
    nvs_set_str(handle, "last_fw_ver", current_version);
    nvs_commit(handle);
    nvs_close(handle);

    return HONCH_OK;
}
