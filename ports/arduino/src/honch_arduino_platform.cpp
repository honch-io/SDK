#include "honch_arduino_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Esp.h>
#include <esp_system.h>
#include <time.h>
#endif

#ifndef ARDUINO
namespace {
uint64_t g_hostNowMs = 1700000000000ULL;
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
  return g_hostNowMs;
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

honch_status_t honch_arduino_platform_ops_init(
    honch_platform_ops_t *ops,
    honch_arduino_platform_t *ctx) {
  if (ops == nullptr || ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  *ops = honch_platform_ops_t{
      honch_arduino_epoch_millis,
      honch_arduino_epoch_millis,
      honch_arduino_random_bytes,
      honch_arduino_log,
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
  g_hostNowMs = nowMs;
}

void honch_arduino_host_advance_millis(uint64_t deltaMs) {
  g_hostNowMs += deltaMs;
}
#endif
