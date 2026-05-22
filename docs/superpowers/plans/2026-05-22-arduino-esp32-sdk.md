# Arduino ESP32 SDK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a first-class Arduino ESP32 SDK port derived from the canonical Honch C core, with a small Arduino-style API, durable queueing, WiFi HTTPS upload to `/capture`, examples, and verification that does not fork analytics behavior from `core/`.

**Architecture:** The Arduino SDK is a thin C++ wrapper plus ESP32 platform adapters around the existing C core. The core remains responsible for event encoding, lifecycle events, queue policy, retry classification, compact wire-v2 payloads, and public analytics semantics. Arduino code owns ergonomics, WiFi/TLS transport wiring, Preferences/NVS storage, time/random/device identity adapters, and examples.

**Tech Stack:** Arduino C++, ESP32 Arduino core, WiFi/WiFiClientSecure/HTTPClient, Preferences/NVS, existing Honch C core, host CMake tests where possible, Arduino CLI compile checks when available.

---

## Product Definition

The first Arduino SDK should target ESP32 boards using the Arduino framework. It should be easy to install locally as an Arduino library and should feel native to Arduino users while preserving the canonical core contract.

Production definition for the first milestone:

- Public API exposes `Honch.begin(config)`, `Honch.track(name, json)`, `Honch.identify(id, json)`, `Honch.setProperty(key, json)`, `Honch.sessionStart(name)`, `Honch.sessionEnd()`, `Honch.flush()`, `Honch.shutdown()`, and `Honch.reset()`.
- Uploads use `POST /capture` with `Content-Type: application/vnd.honch.chunk` and `X-Honch-Project-Key`.
- No Arduino path implements its own event format, queue policy, lifecycle semantics, or wire encoder.
- Queue state survives reboot using ESP32 Preferences/NVS.
- Events can be staged before WiFi is connected and flushed after connectivity returns.
- Minimal example compiles for an ESP32 board.
- Tests or compile checks prove the wrapper calls the core and the transport posts compact chunks.

Out of scope for the first milestone:

- BLE relay peripheral behavior.
- Non-ESP32 Arduino boards.
- Arduino Library Manager publishing metadata beyond a valid `library.properties`.
- OTA integration.
- Custom property builder DSL. JSON strings are enough for v0.

## File Structure

Create:

- `ports/arduino/README.md`: install, setup, examples, limitations.
- `ports/arduino/library.properties`: Arduino library metadata.
- `ports/arduino/src/Honch.h`: Arduino public API.
- `ports/arduino/src/Honch.cpp`: C++ wrapper implementation.
- `ports/arduino/src/honch_arduino_adapter.h`: private adapter declarations.
- `ports/arduino/src/honch_arduino_platform.cpp`: time, random, device ID, logging, allocation glue.
- `ports/arduino/src/honch_arduino_storage.cpp`: Preferences-backed storage and queue adapter.
- `ports/arduino/src/honch_arduino_transport.cpp`: WiFiClientSecure/HTTPClient upload transport.
- `ports/arduino/examples/HonchBasic/HonchBasic.ino`: minimal WiFi + Honch example.
- `ports/arduino/examples/HonchOfflineQueue/HonchOfflineQueue.ino`: stage before WiFi and flush later.
- `ports/arduino/test/host/test_arduino_wrapper.cpp`: host-side wrapper test using fake adapters.
- `ports/arduino/test/host/CMakeLists.txt`: host test target.
- `ports/arduino/scripts/verify-arduino.sh`: compile/check entrypoint.

Modify:

- `README.md`: add Arduino SDK to the repo port list.
- `CMakeLists.txt`: include Arduino host tests only when explicitly enabled.
- `.gitignore` if Arduino CLI build output needs local ignores.

## API Shape

Public header sketch:

```cpp
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
```

Mapping rules:

- `bool` returns `true` only when the underlying core returns `HONCH_OK`.
- `lastError()` returns `honch_status_string(last_status)`.
- Empty optional JSON defaults to `{}`.
- The caller owns `eventBuffer`.
- `host` defaults should not be hidden in code for v0; examples can use `https://capture.honch.io`.

## Task 1: Scaffold Arduino Port

**Files:**

- Create: `ports/arduino/library.properties`
- Create: `ports/arduino/src/Honch.h`
- Create: `ports/arduino/src/Honch.cpp`
- Create: `ports/arduino/README.md`
- Modify: `README.md`

- [ ] **Step 1: Add Arduino library metadata**

Create `ports/arduino/library.properties`:

```ini
name=Honch
version=0.1.0
author=Honch
maintainer=Honch <support@honch.io>
sentence=Product analytics for connected ESP32 devices.
paragraph=Honch queues events locally and uploads compact capture chunks using the canonical Honch C core.
category=Communication
url=https://github.com/honch-io/honch
architectures=esp32
includes=Honch.h
```

- [ ] **Step 2: Add the public header skeleton**

Create `ports/arduino/src/Honch.h` using the API shape above.

- [ ] **Step 3: Add a minimal wrapper implementation**

Create `ports/arduino/src/Honch.cpp` with stub methods that compile and return `false` until core wiring exists:

```cpp
#include "Honch.h"

HonchClass Honch;

bool HonchClass::begin(const HonchConfig &) { return false; }
bool HonchClass::track(const char *, const char *) { return false; }
bool HonchClass::identify(const char *, const char *) { return false; }
bool HonchClass::setProperty(const char *, const char *) { return false; }
bool HonchClass::sessionStart(const char *) { return false; }
bool HonchClass::sessionEnd() { return false; }
bool HonchClass::flush() { return false; }
bool HonchClass::shutdown() { return false; }
bool HonchClass::reset() { return false; }
const char *HonchClass::deviceId() { return ""; }
const char *HonchClass::lastError() { return "not initialized"; }
```

- [ ] **Step 4: Add README quickstart**

Create `ports/arduino/README.md` with:

```md
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
```

- [ ] **Step 5: Run a metadata smoke check**

Run:

```bash
test -f ports/arduino/library.properties
test -f ports/arduino/src/Honch.h
test -f ports/arduino/src/Honch.cpp
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add README.md ports/arduino
git commit -m "feat(arduino): scaffold esp32 sdk"
```

## Task 2: Core Wrapper Wiring

**Files:**

- Modify: `ports/arduino/src/Honch.h`
- Modify: `ports/arduino/src/Honch.cpp`
- Create: `ports/arduino/src/honch_arduino_adapter.h`
- Create: `ports/arduino/test/host/test_arduino_wrapper.cpp`
- Create: `ports/arduino/test/host/CMakeLists.txt`

- [ ] **Step 1: Write a failing host wrapper test**

Create `ports/arduino/test/host/test_arduino_wrapper.cpp`:

```cpp
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
```

- [ ] **Step 2: Run test to verify failure**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: compile or assertion failure because wrapper methods are still stubs.

- [ ] **Step 3: Add private adapter contract**

Create `ports/arduino/src/honch_arduino_adapter.h`:

```cpp
#pragma once

#include "Honch.h"
#include "honch/core/config.h"

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config);
void honch_arduino_release_core_config(honch_core_config_t *config);
```

- [ ] **Step 4: Wire wrapper to core**

Update `ports/arduino/src/Honch.cpp` so `HonchClass` stores:

```cpp
private:
  honch_client_t *_client;
  honch_status_t _lastStatus;
```

and maps methods to:

```cpp
honch_core_init(&_client, &coreConfig);
honch_core_track(_client, eventName, propertiesJson);
honch_core_identify(_client, distinctId, traitsJson);
honch_core_set_property(_client, key, valueJson);
honch_core_session_start(_client, sessionName);
honch_core_session_end(_client);
honch_core_flush(_client);
honch_core_shutdown(_client);
honch_core_reset(_client);
honch_core_get_device_id(_client);
honch_status_string(_lastStatus);
```

- [ ] **Step 5: Run wrapper test**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add ports/arduino/src ports/arduino/test/host
git commit -m "feat(arduino): wrap canonical core api"
```

## Task 3: ESP32 Platform Adapter

**Files:**

- Create: `ports/arduino/src/honch_arduino_platform.cpp`
- Modify: `ports/arduino/src/honch_arduino_adapter.h`
- Test: `ports/arduino/test/host/test_arduino_wrapper.cpp`

- [ ] **Step 1: Add host-testable platform hooks**

Expose C-compatible hooks for:

```cpp
uint64_t honch_arduino_epoch_millis(void *ctx);
honch_status_t honch_arduino_random_bytes(void *ctx, uint8_t *buffer, size_t bufferSize);
honch_status_t honch_arduino_device_id(void *ctx, char *buffer, size_t bufferSize);
void honch_arduino_log(void *ctx, honch_log_level_t level, const char *message);
```

- [ ] **Step 2: Implement ESP32 behavior**

In Arduino builds:

- `epoch_millis`: use `time(nullptr)` when valid, otherwise `millis()`.
- `random_bytes`: use `esp_fill_random`.
- `device_id`: use `ESP.getEfuseMac()` and format as `esp32-XXXXXXXXXXXX`.
- `log`: write to `Serial` only when initialized or compile-time enabled.

- [ ] **Step 3: Keep host behavior deterministic**

For host tests, return:

```text
epoch_millis = 1700000000000
random bytes = 0, 1, 2, ...
device_id = host-arduino-device
```

- [ ] **Step 4: Run host wrapper test**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ports/arduino/src/honch_arduino_platform.cpp ports/arduino/src/honch_arduino_adapter.h ports/arduino/test/host
git commit -m "feat(arduino): add esp32 platform adapter"
```

## Task 4: Preferences/NVS Durable Storage

**Files:**

- Create: `ports/arduino/src/honch_arduino_storage.cpp`
- Modify: `ports/arduino/src/honch_arduino_adapter.h`
- Test: `ports/arduino/test/host/test_arduino_wrapper.cpp`

- [ ] **Step 1: Write queue persistence test**

Extend the host test so it:

1. Calls `Honch.begin(config)`.
2. Calls `Honch.track("queued", "{}")`.
3. Calls `Honch.shutdown()` without successful upload.
4. Calls `Honch.begin(config)` again.
5. Calls `Honch.flush()`.
6. Asserts fake transport received the queued event.

- [ ] **Step 2: Verify failure**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: FAIL because storage does not persist queue state yet.

- [ ] **Step 3: Implement storage adapter**

Use ESP32 `Preferences` in Arduino builds:

- namespace: `honch`
- state keys: match the core storage contract
- queue entries: store sequence-keyed blobs
- queue depth/read/write cursors: store as numeric values

Use an in-memory map for host tests.

- [ ] **Step 4: Run storage test**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ports/arduino/src/honch_arduino_storage.cpp ports/arduino/src/honch_arduino_adapter.h ports/arduino/test/host
git commit -m "feat(arduino): persist queue in preferences"
```

## Task 5: HTTP Capture Transport

**Files:**

- Create: `ports/arduino/src/honch_arduino_transport.cpp`
- Modify: `ports/arduino/src/honch_arduino_adapter.h`
- Test: `ports/arduino/test/host/test_arduino_wrapper.cpp`

- [ ] **Step 1: Write transport contract test**

Extend the host test fake transport to assert:

```text
URL path: /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: test-key
body length > 0
body is already compact wire-v2 bytes
```

- [ ] **Step 2: Verify failure**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: FAIL because Arduino transport is not implemented.

- [ ] **Step 3: Implement Arduino HTTP transport**

In `honch_arduino_transport.cpp`:

- Use `WiFiClientSecure` for `https://`.
- Use `WiFiClient` for `http://`.
- Use `HTTPClient`.
- POST to `${host}/capture`.
- Set headers:
  - `Content-Type: application/vnd.honch.chunk`
  - `X-Honch-Project-Key: <apiKey>`
  - `X-Honch-Stream-Id: <core stream id>`
- Map `2xx` to consumed, `400/401/404` to rejected/drop, and retryable network/5xx/429 statuses to retry.

- [ ] **Step 4: Run transport tests**

Run:

```bash
cmake -S ports/arduino/test/host -B build/arduino-host
cmake --build build/arduino-host
./build/arduino-host/honch_arduino_wrapper_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ports/arduino/src/honch_arduino_transport.cpp ports/arduino/src/honch_arduino_adapter.h ports/arduino/test/host
git commit -m "feat(arduino): upload capture chunks over http"
```

## Task 6: Arduino Examples

**Files:**

- Create: `ports/arduino/examples/HonchBasic/HonchBasic.ino`
- Create: `ports/arduino/examples/HonchOfflineQueue/HonchOfflineQueue.ino`
- Modify: `ports/arduino/README.md`

- [ ] **Step 1: Add basic example**

Create `HonchBasic.ino` with WiFi setup, `Honch.begin`, one `track`, and `flush`.

- [ ] **Step 2: Add offline queue example**

Create `HonchOfflineQueue.ino` that calls `Honch.begin`, tracks before WiFi is connected, then connects WiFi and calls `Honch.flush`.

- [ ] **Step 3: Document example usage**

Update `ports/arduino/README.md` with:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchBasic
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchOfflineQueue
```

- [ ] **Step 4: Run compile checks when Arduino CLI is installed**

Run:

```bash
arduino-cli version
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchBasic
arduino-cli compile --fqbn esp32:esp32:esp32 ports/arduino/examples/HonchOfflineQueue
```

Expected: both examples compile. If `arduino-cli` is missing, document the missing tool in the final verification notes.

- [ ] **Step 5: Commit**

```bash
git add ports/arduino/examples ports/arduino/README.md
git commit -m "docs(arduino): add esp32 examples"
```

## Task 7: Verification Script And Readiness Notes

**Files:**

- Create: `ports/arduino/scripts/verify-arduino.sh`
- Modify: `ports/arduino/README.md`
- Modify: `README.md`

- [ ] **Step 1: Add verification script**

Create `ports/arduino/scripts/verify-arduino.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

cmake -S "$ROOT_DIR/ports/arduino/test/host" -B "$ROOT_DIR/build/arduino-host"
cmake --build "$ROOT_DIR/build/arduino-host"
"$ROOT_DIR/build/arduino-host/honch_arduino_wrapper_test"

if command -v arduino-cli >/dev/null 2>&1; then
  arduino-cli compile --fqbn esp32:esp32:esp32 "$ROOT_DIR/ports/arduino/examples/HonchBasic"
  arduino-cli compile --fqbn esp32:esp32:esp32 "$ROOT_DIR/ports/arduino/examples/HonchOfflineQueue"
else
  echo "arduino-cli not found; skipped Arduino example compile checks" >&2
fi
```

- [ ] **Step 2: Make script executable**

Run:

```bash
chmod +x ports/arduino/scripts/verify-arduino.sh
```

- [ ] **Step 3: Run verification**

Run:

```bash
ports/arduino/scripts/verify-arduino.sh
```

Expected: host tests pass; Arduino example compile checks pass or are explicitly skipped because `arduino-cli` is missing.

- [ ] **Step 4: Update docs with readiness status**

Update `ports/arduino/README.md` with:

- supported board family: ESP32 Arduino
- required dependencies: ESP32 Arduino core, WiFi, Preferences, HTTPClient
- verification command
- known limitations from the first milestone

- [ ] **Step 5: Commit**

```bash
git add README.md ports/arduino/README.md ports/arduino/scripts/verify-arduino.sh
git commit -m "chore(arduino): add verification gate"
```

## Final Acceptance Checklist

- [ ] Arduino SDK uses the canonical C core for analytics behavior.
- [ ] Public Arduino API is small and idiomatic.
- [ ] ESP32 platform adapter provides time, random bytes, device ID, and logging.
- [ ] Preferences/NVS storage preserves queued events across restart.
- [ ] HTTP transport posts compact chunks to `/capture`.
- [ ] Basic and offline examples exist.
- [ ] Host tests pass.
- [ ] Arduino CLI compile checks pass or missing CLI is explicitly documented.
- [ ] README documents setup, examples, verification, and limitations.
- [ ] No Arduino code introduces a separate event format or wire encoder.
