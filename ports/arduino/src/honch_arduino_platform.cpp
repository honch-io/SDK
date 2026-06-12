#include "honch_arduino_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Esp.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#else
#include <chrono>
#include <mutex>
#include <new>
#endif

#define HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS 10u

#ifndef ARDUINO
namespace {
uint64_t g_hostUptimeMs = 1700000000000ULL;
uint64_t g_hostEpochMs = 0ULL;
bool g_hostEpochValid = false;
}
#endif

uint64_t honch_arduino_epoch_millis(void *ctx) {
  (void)ctx;
#ifdef ARDUINO
  static const uint64_t HONCH_ARDUINO_MIN_UNIX_TIME_SECONDS = 1577836800ULL;
  time_t now = time(nullptr);
  if (now >= (time_t)HONCH_ARDUINO_MIN_UNIX_TIME_SECONDS) {
    return (uint64_t)now * 1000ULL;
  }
  return (uint64_t)millis();
#else
  return g_hostEpochValid ? g_hostEpochMs : g_hostUptimeMs;
#endif
}

uint64_t honch_arduino_uptime_millis(void *ctx) {
  (void)ctx;
#ifdef ARDUINO
  return (uint64_t)millis();
#else
  return g_hostUptimeMs;
#endif
}

honch_status_t honch_arduino_random_bytes(void *ctx, uint8_t *buffer, size_t bufferSize) {
  (void)ctx;
  if (buffer == nullptr && bufferSize > 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
#ifdef ARDUINO
  if (bufferSize > 0) {
    esp_fill_random(buffer, bufferSize);
  }
#else
  for (size_t i = 0; i < bufferSize; ++i) {
    buffer[i] = (uint8_t)(i & 0xffu);
  }
#endif
  return HONCH_OK;
}

honch_status_t honch_arduino_device_id(void *ctx, char *buffer, size_t bufferSize) {
  (void)ctx;
  if (buffer == nullptr || bufferSize == 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
#ifdef ARDUINO
  uint64_t mac = ESP.getEfuseMac();
  int written = snprintf(
      buffer,
      bufferSize,
      "esp32-%012llX",
      (unsigned long long)mac);
#else
  int written = snprintf(buffer, bufferSize, "%s", "host-arduino-device");
#endif
  if (written < 0 || (size_t)written >= bufferSize) {
    buffer[0] = '\0';
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  return HONCH_OK;
}

static honch_fault_snapshot_t honch_arduino_reset_snapshot(
    honch_fault_kind_t kind,
    honch_fault_severity_t severity,
    const char *reset_reason) {
  honch_fault_snapshot_t snapshot = {};
  snapshot.kind = kind;
  snapshot.severity = severity;
  snapshot.reset_reason = reset_reason;
  return snapshot;
}

honch_fault_snapshot_t honch_arduino_fault_snapshot() {
#ifdef ARDUINO
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_NONE,
          HONCH_FAULT_SEVERITY_INFO,
          "power_on");
    case ESP_RST_SW:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_NONE,
          HONCH_FAULT_SEVERITY_INFO,
          "software");
    case ESP_RST_DEEPSLEEP:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_NONE,
          HONCH_FAULT_SEVERITY_INFO,
          "deep_sleep");
    case ESP_RST_PANIC:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_PANIC,
          HONCH_FAULT_SEVERITY_FATAL,
          "panic");
    case ESP_RST_INT_WDT:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_WATCHDOG,
          HONCH_FAULT_SEVERITY_FATAL,
          "interrupt_wdt");
    case ESP_RST_TASK_WDT:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_WATCHDOG,
          HONCH_FAULT_SEVERITY_FATAL,
          "task_wdt");
    case ESP_RST_WDT:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_WATCHDOG,
          HONCH_FAULT_SEVERITY_FATAL,
          "watchdog");
    case ESP_RST_BROWNOUT:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_BROWNOUT,
          HONCH_FAULT_SEVERITY_FATAL,
          "brownout");
    case ESP_RST_SDIO:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_NONE,
          HONCH_FAULT_SEVERITY_INFO,
          "sdio");
    case ESP_RST_EXT:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_NONE,
          HONCH_FAULT_SEVERITY_INFO,
          "external");
    case ESP_RST_UNKNOWN:
    default:
      return honch_arduino_reset_snapshot(
          HONCH_FAULT_KIND_UNKNOWN,
          HONCH_FAULT_SEVERITY_FATAL,
          "unknown");
  }
#else
  return honch_arduino_reset_snapshot(
      HONCH_FAULT_KIND_NONE,
      HONCH_FAULT_SEVERITY_INFO,
      "host");
#endif
}

void honch_arduino_log(void *ctx, honch_log_level_t level, const char *message) {
  (void)ctx;
  (void)level;
#ifdef ARDUINO
  if (message != nullptr) {
    Serial.println(message);
  }
#else
  (void)message;
#endif
}

honch_status_t honch_arduino_mutex_create(void *ctx, void **mutex) {
  (void)ctx;
  if (mutex == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
#ifdef ARDUINO
  SemaphoreHandle_t handle = xSemaphoreCreateMutex();
  if (handle == nullptr) {
    return HONCH_ERROR_OUT_OF_MEMORY;
  }
  *mutex = handle;
#else
  std::timed_mutex *handle = new (std::nothrow) std::timed_mutex();
  if (handle == nullptr) {
    return HONCH_ERROR_OUT_OF_MEMORY;
  }
  *mutex = handle;
#endif
  return HONCH_OK;
}

void honch_arduino_mutex_destroy(void *ctx, void *mutex) {
  (void)ctx;
  if (mutex == nullptr) {
    return;
  }
#ifdef ARDUINO
  vSemaphoreDelete((SemaphoreHandle_t)mutex);
#else
  delete static_cast<std::timed_mutex *>(mutex);
#endif
}

honch_status_t honch_arduino_mutex_lock(void *ctx, void *mutex) {
  (void)ctx;
  if (mutex == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
#ifdef ARDUINO
  return xSemaphoreTake((SemaphoreHandle_t)mutex, pdMS_TO_TICKS(HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS)) == pdTRUE ?
      HONCH_OK :
      HONCH_ERROR_BUSY;
#else
  return static_cast<std::timed_mutex *>(mutex)->try_lock_for(
      std::chrono::milliseconds(HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS)) ?
      HONCH_OK :
      HONCH_ERROR_BUSY;
#endif
}

void honch_arduino_mutex_unlock(void *ctx, void *mutex) {
  (void)ctx;
  if (mutex == nullptr) {
    return;
  }
#ifdef ARDUINO
  (void)xSemaphoreGive((SemaphoreHandle_t)mutex);
#else
  static_cast<std::timed_mutex *>(mutex)->unlock();
#endif
}

honch_status_t honch_arduino_platform_ops_init(
    honch_platform_ops_t *ops,
    honch_arduino_platform_t *ctx) {
  if (ops == nullptr || ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  *ops = honch_platform_ops_t{
      honch_arduino_epoch_millis,
      honch_arduino_uptime_millis,
      honch_arduino_random_bytes,
      honch_arduino_log,
      honch_arduino_mutex_create,
      honch_arduino_mutex_destroy,
      honch_arduino_mutex_lock,
      honch_arduino_mutex_unlock,
      1u,
      ctx,
  };
  return HONCH_OK;
}

extern "C" {

uint64_t honch_now_millis(void) {
  return honch_arduino_epoch_millis(nullptr);
}

honch_status_t honch_random_hex(char out[33]) {
  if (out == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  uint8_t bytes[16];
  honch_status_t status = honch_arduino_random_bytes(nullptr, bytes, sizeof(bytes));
  if (status != HONCH_OK) {
    return status;
  }

  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
    out[(i * 2) + 1] = hex[bytes[i] & 0x0f];
  }
  out[32] = '\0';
  return HONCH_OK;
}

}

#ifndef ARDUINO
void honch_arduino_host_set_millis(uint64_t nowMs) {
  g_hostUptimeMs = nowMs;
  g_hostEpochValid = false;
}

void honch_arduino_host_advance_millis(uint64_t deltaMs) {
  g_hostUptimeMs += deltaMs;
  if (g_hostEpochValid) {
    g_hostEpochMs += deltaMs;
  }
}

void honch_arduino_host_set_epoch_millis(uint64_t nowMs) {
  g_hostEpochMs = nowMs;
  g_hostEpochValid = true;
}

void honch_arduino_host_clear_epoch_millis(void) {
  g_hostEpochValid = false;
}
#endif
