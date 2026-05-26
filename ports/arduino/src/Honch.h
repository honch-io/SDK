#pragma once

#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"

struct HonchConfig {
  const char *apiKey;
  const char *host;
  const char *rootCaPem;
  const char *deviceModel;
  const char *firmwareVersion;
  const char *environment;
  uint8_t *eventBuffer;
  size_t eventBufferSize;
  uint32_t flushIntervalSeconds;
  uint32_t flushEventThreshold;
  bool insecureSkipTlsVerify;
};

class HonchClass {
public:
  HonchClass();
  bool begin(const HonchConfig &config);
  bool track(const char *eventName, const char *propertiesJson = "{}");
  bool identify(const char *distinctId, const char *traitsJson = "{}");
  bool setProperty(const char *key, const char *valueJson);
  bool sessionStart(const char *sessionName);
  bool sessionEnd();
  bool flush();
  bool tick();
  bool loop();
  bool shutdown();
  bool reset();
  const char *deviceId();
  const char *lastError();

private:
  bool setLastStatus(honch_status_t status);
  bool recordQueuedStatus(honch_status_t status);
  void resetScheduler(uint64_t nowMs);
  bool schedulerDue(uint64_t nowMs) const;
  void recordSchedulerFlushResult(honch_status_t status, uint64_t nowMs);

  honch_client_t *_client;
  honch_status_t _lastStatus;
  uint32_t _flushIntervalSeconds;
  uint32_t _flushEventThreshold;
  uint32_t _queuedSinceFlush;
  uint64_t _nextIntervalFlushMs;
  uint64_t _nextRetryFlushMs;
  uint32_t _retryDelayMs;
};

extern HonchClass Honch;
