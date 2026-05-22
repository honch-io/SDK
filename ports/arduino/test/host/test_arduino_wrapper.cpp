#include <assert.h>
#include <string.h>

#include "../../src/Honch.h"

int main() {
  static uint8_t buffer[8192];
  HonchConfig config = {
    .apiKey = "test-key",
    .host = "http://127.0.0.1:8001",
    .deviceModel = "host-esp32",
    .firmwareVersion = "1.0.0",
    .environment = "test",
    .eventBuffer = buffer,
    .eventBufferSize = sizeof(buffer),
    .flushIntervalSeconds = 60,
    .flushEventThreshold = 30,
    .insecureSkipTlsVerify = true,
  };

  assert(Honch.begin(config));
  assert(Honch.track("button_pressed", "{\"count\":1}"));
  assert(Honch.identify("user-1", "{\"role\":\"tester\"}"));
  assert(Honch.setProperty("mode", "\"host\""));
  assert(Honch.sessionStart("demo"));
  assert(Honch.sessionEnd());
  assert(Honch.flush());
  assert(strlen(Honch.deviceId()) > 0);
  assert(strcmp(Honch.lastError(), "ok") == 0);
  assert(Honch.shutdown());
  return 0;
}
