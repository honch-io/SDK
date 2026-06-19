# Honch Arduino ESP32 SDK

Preview Arduino ESP32 wrapper around the canonical Honch C core.

## Status

Preview `0.2.3`. Use for evaluation or controlled pilots until your product has passed hardware, TLS, offline queue, flush, retry, and power-cycle validation on the target ESP32 board.

That version (above) is what's published in `library.properties` and reported by
events in `$sdk_version`. The Arduino wrapper, the vendored C core, and the
package metadata share a single version tracked by `HONCH_SDK_VERSION`, matching
the canonical wire/runtime contract used by the other C-derived SDKs.

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
    .host = "https://i.honch.io",
    .rootCaPem = HONCH_ROOT_CA_PEM,
    .deviceModel = "esp32-devkit",
    .firmwareVersion = "1.0.0",
    .environment = "production",
    .eventBuffer = eventBuffer,
    .eventBufferSize = sizeof(eventBuffer),
    .flushIntervalSeconds = 120,
    .flushMinIntervalMs = 15000,
    .flushEventThreshold = 20,
    .insecureSkipTlsVerify = false,
  };

  if (honch::defaultClient().begin(config)) {
    const honch_property_t properties[] = {
      honch_prop("source", honch_str("setup")),
    };
    honch::defaultClient().track("app_started", properties, 1);
  }
}

void loop() {
  honch::defaultClient().tick();
}
```

`honch::defaultClient().begin()` init does synchronous work on the caller's
task. It validates config, initializes the C core against the caller-provided
event buffer, reads the ESP reset reason, derives initial state from the
supplied device/config values, and queues `$device_boot` before returning. It
does not perform network I/O;
delivery remains cooperative through `honch::defaultClient().tick()` or
explicit flush calls.

Automatic `$error` capture is disabled by default. When
`config.enableErrorTracking` is true, the wrapper maps panic,
interrupt/task/other watchdog, brownout, and unknown reset reasons from
`esp_reset_reason()` into a bounded `$error` event with `source`, `severity`,
and `reset_reason` properties during `begin()`. It does not install panic
handlers, save coredumps, collect registers, copy stacks, upload symbol files,
send source code/source paths, or replace ESP32 crash forensics tooling.
Define `HONCH_ENABLE_ERROR_TRACKING=0` in the Arduino build flags to compile
out the wrapper reset-snapshot path entirely.

Call `honch::defaultClient().reportError(report, properties, propertyCount)`
from normal runtime error paths to queue `$error` with `source="runtime"`.
Delivery follows the same cooperative `tick()`/`flush()` policy as `track()`.

All `HonchClass` methods serialize access to the wrapper-owned client pointer.
Concurrent calls from multiple FreeRTOS tasks are allowed, but only one wrapper
call runs at a time. If another task is already inside the wrapper, the waiting
call uses the SDK's bounded mutex wait and returns false with `lastError()` set
to `busy` when it cannot acquire the lock. Keep using one low-priority pump task
for `tick()` / `flush()` so application work is not blocked behind network I/O.
Pointers returned by `deviceId()` are borrowed from the core client and remain
valid only until the next successful `reset()` or `shutdown()`.

`honch::defaultClient().tick()` performs scheduled flush work when the interval elapses or the
event threshold is reached. Successful outbound uploads are spaced by
`flushMinIntervalMs`; leave it at the 15000 ms default for consumer firmware, or
set it to `HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS` for benchmark or explicit
high-throughput modes.
`transportTimeoutMs` bounds each synchronous HTTP POST; leave it at the 2500 ms
default unless the capture endpoint and network need a different per-request
timeout. Values above the hard maximum of 10000 ms are clamped.

`honch::defaultClient().tick()` may block for up to the configured transport timeout because the
HTTP POST is synchronous and runs on the caller's task. Do not call
`honch::defaultClient().tick()` from a latency-sensitive control loop. Do not
call `honch::defaultClient().tick()` or `honch::defaultClient().flush()` from an
ISR, high-priority task, motor-control path, sensor sampling deadline, UI
refresh path, or watchdog-sensitive section. The SDK does not start a hidden
background task. Use `honch::defaultClient().loop()` as an alias only when the
sketch's `loop()` can tolerate upload latency.
HTTPS uploads allocate the TLS client lazily on the heap, but the ESP32 Arduino
HTTP/TLS stack still needs a pump task with enough stack for the handshake and
POST path. Use at least an 8192 byte stack for a dedicated HTTPS pump task unless
your firmware has measured a smaller board-specific limit.
Each scheduled tick posts at most one wire chunk, so large queued uploads may
need several pump iterations to finish.

For firmware with latency-sensitive `loop()` work, pump Honch from a dedicated
low-priority FreeRTOS task:

```cpp
static void honchPumpTask(void *parameter) {
  (void)parameter;
  for (;;) {
    honch::defaultClient().tick();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void setup() {
  // Configure Wi-Fi and call honch::defaultClient().begin(config) first.
  xTaskCreatePinnedToCore(honchPumpTask, "honch-pump", 8192, nullptr, 1, nullptr, 1);
}
```

Do not call `honch::defaultClient().tick()` while Wi-Fi is unavailable or the radio is
intentionally off. If the sketch cannot guarantee that, set
`connectivityCallback`; when it returns false, ticks keep uploads pending and
`honch::defaultClient().flush()` returns false with `lastError()` set to `offline` without
network I/O.

The default queue uses only the caller-provided RAM buffer. Events and
`identify()` state are lost across reset or power loss unless you provide
`eventQueueOps` and/or `stateStorageOps` backed by durable storage.

After `identify()`, future events use the new distinct ID. The `$identify`
event also includes the previous identity as `$anon_distinct_id`, usually the
device ID, so earlier anonymous events can merge into the identified person.

That RAM queue is bounded and compact: consuming a non-tail event uses an O(n)
memmove per consumed event to keep the caller-provided buffer contiguous. This
is acceptable at default sizes, but larger buffers and queue limits should be
measured on the target board.

`maxQueuedEvents` is a soft ceiling, not a guarantee: real capacity is bounded by
the RAM buffer (and, if supplied, the durable tier), which may be well below it.
The core treats it as an upper bound on the queued count; drop-oldest still
applies once the tiers actually fill.

If you wire a durable queue and call `honch_tiered_queue_persist()` yourself (for
example to flush to flash on shutdown), call it with the same mutual exclusion as
`track()`/`tick()`: it drives the RAM queue lock-free, so it must never run while
another task is touching the queue, or the shared RAM buffer is corrupted.

## Security

Use HTTPS in production. Configure `rootCaPem` for the Capture endpoint. `insecureSkipTlsVerify` exists only for intentional local testing and should remain `false` in production.

## Examples

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchBasic
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchDedicatedTask
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
