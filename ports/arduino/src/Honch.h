#pragma once

#include <stddef.h>
#include <stdint.h>

struct HonchConfig {
  const char *apiKey;
  const char *host;
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
  bool begin(const HonchConfig &config);
  bool track(const char *eventName, const char *propertiesJson = "{}");
  bool identify(const char *distinctId, const char *traitsJson = "{}");
  bool setProperty(const char *key, const char *valueJson);
  bool sessionStart(const char *sessionName);
  bool sessionEnd();
  bool flush();
  bool shutdown();
  bool reset();
  const char *deviceId();
  const char *lastError();
};

extern HonchClass Honch;
