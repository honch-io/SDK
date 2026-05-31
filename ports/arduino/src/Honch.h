#pragma once

#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"

struct HonchConfig {
  const char *apiKey;
  const char *host;
  const char *rootCaPem;
  const char *deviceId;
  const char *deviceModel;
  const char *firmwareVersion;
  const char *environment;
  uint8_t *eventBuffer;
  size_t eventBufferSize;
  uint32_t flushIntervalSeconds;
  uint32_t flushEventThreshold;
  bool insecureSkipTlsVerify;
  const honch_state_storage_ops_t *stateStorageOps;
  const honch_event_queue_ops_t *eventQueueOps;
};

class HonchClass {
public:
  HonchClass();
  bool begin(const HonchConfig &config);
  bool track(const char *eventName, const honch_property_t *properties = nullptr, size_t propertyCount = 0);
  bool identify(const char *distinctId, const honch_property_t *traits = nullptr, size_t traitCount = 0);
  bool setProperty(const char *key, honch_value_t value);
  bool sessionStart(const char *sessionName);
  bool sessionEnd();
  bool flush();
  bool tick();
  bool loop();
  bool shutdown();
  bool reset();
  const char *deviceId();
  bool queueStats(honch_queue_stats_t *stats);
  const char *lastError();

private:
  bool setLastStatus(honch_status_t status);
  bool recordQueuedStatus(honch_status_t status);

  honch_client_t *_client;
  honch_status_t _lastStatus;
};

extern HonchClass Honch;
