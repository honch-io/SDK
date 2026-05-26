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
static const char HONCH_ROOT_CA_PEM[] = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_CAPTURE_ENDPOINT_ROOT_CA
-----END CERTIFICATE-----
)EOF";

void setup() {
  WiFi.begin("ssid", "password");
  while (WiFi.status() != WL_CONNECTED) delay(250);

  HonchConfig config = {
    .apiKey = "project-key",
    .host = "https://capture.honch.io",
    .rootCaPem = HONCH_ROOT_CA_PEM,
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

Or compile the PlatformIO example project:

```sh
pio run -d ports/arduino/examples/platformio
```

## Verification

From the repository root:

```sh
ports/arduino/scripts/verify-arduino.sh
```

The script always runs the host wrapper test. If `arduino-cli` is installed, it
also compiles the ESP32 examples; otherwise it reports that the Arduino compile
checks were skipped.

For release-style verification, require `arduino-cli` and keep Arduino CLI data
on an external/cache volume:

```sh
HONCH_ARDUINO_HOME="/Volumes/X9 Pro/honch-arduino-verify" \
  ports/arduino/scripts/verify-arduino.sh --require-arduino-cli
```

## Limitations

- ESP32 Arduino is the only supported Arduino board family.
- BLE relay, OTA integration, and non-ESP32 boards are not part of this milestone.
- JSON strings are the v0 property interface; no Arduino property-builder DSL is included.
- TLS root CA PEM configuration is application-owned via `rootCaPem`.
  `insecureSkipTlsVerify` exists for local testing only.
