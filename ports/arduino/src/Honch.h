#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "honch/core/honch.h"

struct HonchConfig {
  const char *apiKey = nullptr;
  const char *host = nullptr;
  const char *rootCaPem = nullptr;
  const char *deviceId = nullptr;
  const char *deviceModel = nullptr;
  const char *firmwareVersion = nullptr;
  const char *environment = nullptr;
  uint8_t *eventBuffer = nullptr;
  size_t eventBufferSize = 0;
  uint32_t flushIntervalSeconds = 0;
  uint32_t flushMinIntervalMs = 0;
  uint32_t flushEventThreshold = 0;
  bool (*connectivityCallback)() = nullptr;
  bool enableErrorTracking = false;
  bool insecureSkipTlsVerify = false;
  const honch_state_storage_ops_t *stateStorageOps = nullptr;
  const honch_event_queue_ops_t *eventQueueOps = nullptr;
  uint32_t transportTimeoutMs = 0;
};

class HonchClass {
public:
  HonchClass();
  ~HonchClass();
  HonchClass(const HonchClass &) = delete;
  HonchClass &operator=(const HonchClass &) = delete;
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
  // Structured detail for the most recent failure (status + reason + http_status
  // + os_error + message). Returns false if unavailable. lastError() is unchanged.
  bool lastErrorDetail(honch_error_detail_t *out);
  // One human-readable line for the most recent failure, e.g.
  // "rejected: HTTP 401 - API key invalid or revoked (reason=auth_invalid_key)".
  const char *lastErrorMessage();

#ifndef ARDUINO
  bool hostLockForTest();
  void hostUnlockForTest();
#endif

private:
  honch_status_t lockInstance();
  void unlockInstance();
  bool setLastStatusLocked(honch_status_t status);

  honch_client_t *_client;
  void *_instanceMutex;
  std::atomic<honch_status_t> _lastStatus;
  char _lastErrorMessage[192];
};

namespace honch {

HonchClass &defaultClient();

} // namespace honch
