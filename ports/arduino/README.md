# Honch Arduino ESP32 SDK

Preview Arduino ESP32 wrapper around the canonical Honch C core.

## Status

Preview `0.1.0`. Use for evaluation or controlled pilots until your product has passed hardware, TLS, offline queue, flush, retry, and power-cycle validation on the target ESP32 board.

## Support

- Board family: ESP32 Arduino.
- Required dependencies: ESP32 Arduino core, `WiFi`, `HTTPClient`, and `WiFiClientSecure`.
- Upload endpoint: `POST /capture` with compact chunks produced by the shared Honch C core.
- Not supported: non-ESP32 Arduino boards, BLE relay, and OTA integration.

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
    .flushMinIntervalMs = 10000,
    .flushEventThreshold = 30,
    .insecureSkipTlsVerify = false,
  };

  if (Honch.begin(config)) {
    const honch_property_t properties[] = {
      honch_prop("source", honch_str("setup")),
    };
    Honch.track("app_started", properties, 1);
  }
}

void loop() {
  Honch.tick();
}
```

`Honch.tick()` performs scheduled flush work when the interval elapses or the
event threshold is reached. Successful outbound uploads are spaced by
`flushMinIntervalMs`; leave it at the 10000 ms default for consumer firmware, or
set it to `HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS` for benchmark or explicit
high-throughput modes. Do not call `Honch.tick()` while Wi-Fi is unavailable or
the radio is intentionally off. If the sketch cannot guarantee that, set
`connectivityCallback`; when it returns false, ticks keep uploads pending and
`Honch.flush()` returns false with `lastError()` set to `offline` without
network I/O. The SDK does not start a hidden background task. Use `Honch.loop()`
as an alias if that fits the sketch.

The default queue uses only the caller-provided RAM buffer. Events and
`identify()` state are lost across reset or power loss unless you provide
`eventQueueOps` and/or `stateStorageOps` backed by durable storage.

## Security

Use HTTPS in production. Configure `rootCaPem` for the Capture endpoint. `insecureSkipTlsVerify` exists only for intentional local testing and should remain `false` in production.

## Examples

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchBasic
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchOfflineQueue
pio run -d ports/arduino/examples/platformio
```

## Verification

From the repository root:

```bash
ports/arduino/scripts/verify-arduino.sh
```

For release-style verification, require Arduino CLI:

```bash
ports/arduino/scripts/verify-arduino.sh --require-arduino-cli
```

Before production, record board, Arduino core version, TLS setup, queue behavior before Wi-Fi, retry behavior, reset/power-cycle behavior, and Capture acceptance.
