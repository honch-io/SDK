# Honch Arduino ESP32 SDK

Arduino ESP32 wrapper around the canonical Honch C core.

## Status

In progress. The first milestone targets ESP32 boards using the Arduino
framework and has not yet been end-to-end tested on ESP32 hardware.

## Support

- Board family: ESP32 Arduino.
- Required dependencies: ESP32 Arduino core, WiFi, Preferences, HTTPClient.
- Upload endpoint: `POST /capture` with compact chunks produced by the canonical Honch C core.

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

## Verification

From the repository root:

```sh
ports/arduino/scripts/verify-arduino.sh
```

The script always runs the host wrapper test. If `arduino-cli` is installed, it also compiles the ESP32 examples; otherwise it reports that the Arduino compile checks were skipped.

## Limitations

- ESP32 Arduino is the only supported Arduino board family.
- BLE relay, OTA integration, and non-ESP32 boards are not part of this milestone.
- JSON strings are the v0 property interface; no Arduino property-builder DSL is included.
- TLS root CA configuration is application-owned. `insecureSkipTlsVerify` exists for local testing only.
