# Honch Arduino ESP32 SDK

Arduino ESP32 wrapper around the canonical Honch C core.

## Status

Planned. The first milestone targets ESP32 boards using the Arduino framework.

## Minimal Use

```cpp
#include <WiFi.h>
#include <Honch.h>

static uint8_t eventBuffer[8192];

void setup() {
  WiFi.begin("ssid", "password");
  while (WiFi.status() != WL_CONNECTED) delay(250);

  HonchConfig config = {
    .apiKey = "project-key",
    .host = "https://capture.honch.io",
    .deviceModel = "esp32-devkit",
    .firmwareVersion = "1.0.0",
    .environment = "production",
    .eventBuffer = eventBuffer,
    .eventBufferSize = sizeof(eventBuffer),
    .flushIntervalSeconds = 60,
    .flushEventThreshold = 30,
    .insecureSkipTlsVerify = false,
  };

  Honch.begin(config);
  Honch.track("boot", "{}");
  Honch.flush();
}

void loop() {}
```

## Examples

Compile the included examples with Arduino CLI:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchBasic
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchOfflineQueue
```
