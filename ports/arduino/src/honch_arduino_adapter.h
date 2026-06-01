#pragma once

#include "Honch.h"
#include "honch/core/config.h"
#include "honch/core/platform.h"
#include "honch/core/ram_queue.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

struct honch_arduino_platform {
  void *reserved;
};

struct honch_arduino_storage {
  honch_ram_queue_t ramQueue;
};

struct honch_arduino_transport {
  const char *apiKey;
  const char *host;
  const char *rootCaPem;
  uint32_t transportTimeoutMs;
  bool insecureSkipTlsVerify;
};

typedef struct honch_arduino_platform honch_arduino_platform_t;
typedef struct honch_arduino_storage honch_arduino_storage_t;
typedef struct honch_arduino_transport honch_arduino_transport_t;

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config);
void honch_arduino_release_core_config(honch_core_config_t *config);

uint64_t honch_arduino_epoch_millis(void *ctx);
honch_status_t honch_arduino_random_bytes(void *ctx, uint8_t *buffer, size_t bufferSize);
honch_status_t honch_arduino_device_id(void *ctx, char *buffer, size_t bufferSize);
void honch_arduino_log(void *ctx, honch_log_level_t level, const char *message);

honch_status_t honch_arduino_platform_ops_init(
    honch_platform_ops_t *ops,
    honch_arduino_platform_t *ctx);
honch_status_t honch_arduino_storage_ops_init(
    honch_event_queue_ops_t *ops,
    honch_arduino_storage_t *ctx,
    const HonchConfig &config);
void honch_arduino_storage_ops_deinit(honch_arduino_storage_t *ctx);
honch_status_t honch_arduino_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_arduino_transport_t *ctx,
    const HonchConfig &config);

#ifndef ARDUINO
void honch_arduino_host_transport_reset(void);
size_t honch_arduino_host_transport_call_count(void);
const char *honch_arduino_host_transport_last_url(void);
const char *honch_arduino_host_transport_last_content_type(void);
const char *honch_arduino_host_transport_last_project_key(void);
const char *honch_arduino_host_transport_last_stream_id(void);
const uint8_t *honch_arduino_host_transport_last_body(void);
size_t honch_arduino_host_transport_last_body_size(void);
void honch_arduino_host_transport_set_result(
    honch_status_t status,
    honch_transport_result_t result);
honch_status_t honch_arduino_host_classify_http_status(
    int code,
    honch_transport_result_t *result);
void honch_arduino_host_set_millis(uint64_t nowMs);
void honch_arduino_host_advance_millis(uint64_t deltaMs);
void honch_arduino_host_set_epoch_millis(uint64_t nowMs);
void honch_arduino_host_clear_epoch_millis(void);
#endif
