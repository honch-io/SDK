# Honch SDKs

Product analytics SDKs for connected hardware. This repository contains the shared SDK contract, platform SDKs for firmware and embedded Linux, and companion relay packages for forwarding device-originated payloads.

## Device SDK Status

| Platform | Public status | Version | Path |
| --- | --- | --- | --- |
| ESP-IDF | Stable | `0.2.4` | [`ports/esp-idf/`](ports/esp-idf/) |
| C/POSIX | Stable | `0.2.4` | [`ports/posix/`](ports/posix/) |
| MicroPython | Stable | `0.2.4` | [`ports/micropython/`](ports/micropython/) |
| Arduino ESP32 | Preview | `0.2.4` | [`ports/arduino/`](ports/arduino/) |

Stable SDKs are supported integration paths for product work. Preview SDKs are usable for evaluation and controlled pilots, but production rollout should wait for product-specific validation on the target hardware.

## Companion Relay Package Status

| Package | Public status | Version | Path |
| --- | --- | --- | --- |
| React Native Relay | Preview | `0.1.0` | [`mobile/react-native-relay/`](mobile/react-native-relay/) |
| Swift Relay | Preview | `0.1.0` | [`mobile/swift-relay/`](mobile/swift-relay/) |

Relay packages are not application analytics SDKs. They forward device-originated Honch payloads through companion apps, so production rollout should include host-app and target-device validation.

## Runtime Behavior Contract

Honch is not silent on the wire. The SDK stamps required context and can
auto-emit `$device_boot`, `$device_shutdown`, `$firmware_update`,
`$battery_low`, `$session_start`, and `$session_end`; those auto-emitted
events create analytics traffic and queue pressure even when the host only
calls `init`, session, battery, or shutdown APIs. Error and crash reporting is
**automatic** — there is no manual `report_error` API.

The SDK recovers a crash from the previous boot and emits a one-time `$crash`
event during the next `init()` (panic, watchdog, brownout, stack overflow,
assert, lockup, unhandled exception, or fatal signal — fidelity tiered by what
each platform's native fault machinery exposes), and it hooks the platform's
error-log path so an error the firmware logs becomes a bounded, coalesced
`$error` event with no host code change. Both ride the normal queue/flush policy
and never perform immediate network I/O. Capture is on by default and is removed
only at build time. See `spec/auto-properties.md` for the `$crash`/`$error`
contract.

Automatic `$error` capture is lightweight crash telemetry, not coredump
collection. When enabled, ESP-IDF maps platform reset reasons such as panic,
watchdog, brownout, and unknown reset into a bounded `$error` event during
init. ESP-IDF can also optionally include a firmware build identifier and raw
crash addresses for server-side symbolication; the SDK does not send source
code, symbol files, or source paths. Arduino ESP32 maps reset reasons through
the same opt-in automatic path without raw crash addresses. C/POSIX can install
opt-in signal breadcrumbs that are converted to `$error` on the next init.

Automatic error tracking is build-strip modular. CMake and embedded ports may
compile with `HONCH_ENABLE_ERROR_TRACKING=0` to remove the SDK-owned `$error`
emission path from firmware. ESP-IDF also exposes `CONFIG_HONCH_ERROR_TRACKING`
and `CONFIG_HONCH_CRASH_SYMBOLICATION`; disabling crash symbolication removes
the ESP-IDF coredump-summary dependency while preserving normal analytics.

honch_init does synchronous work on the caller's thread. Depending on the
port and configured storage hooks, init may validate config, read or derive a
device ID, read or write state such as firmware version, create or reconcile
queue storage, and queues `$device_boot` before returning. It does not perform
network I/O; uploads remain cooperative through `honch_tick()` or explicit
flush calls.

The default RAM queue is bounded and drop-oldest. Consuming a non-tail event
uses an O(n) memmove per consumed event to compact the caller-provided buffer;
that is acceptable at the default queue sizes, but products with larger custom
queues should measure consume cost on their target hardware.

## Repository Layout

- [`core/`](core/) — canonical portable SDK behavior: typed event properties, HQR1 queue records, compact encoding, identity, lifecycle events, queue policy, retry classification, and packetization.
- [`ports/esp-idf/`](ports/esp-idf/) — ESP-IDF component for ESP32-family firmware.
- [`ports/posix/`](ports/posix/) — C/POSIX SDK for embedded Linux and local validation.
- [`ports/micropython/`](ports/micropython/) — MicroPython wrapper and `_honch_core` user module.
- [`ports/arduino/`](ports/arduino/) — preview Arduino ESP32 wrapper around the shared C core.
- [`mobile/react-native-relay/`](mobile/react-native-relay/) — preview relay package for forwarding device-originated payloads through companion apps.
- [`mobile/swift-relay/`](mobile/swift-relay/) — preview native Swift relay package for forwarding device-originated payloads through companion apps.
- [`spec/`](spec/) — shared SDK/Capture contracts and conformance fixtures.

## Build And Test

### C/POSIX

```bash
cd ports/posix
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### ESP-IDF

Activate ESP-IDF first, then build the example app:

```bash
cd ports/esp-idf/example
idf.py set-target esp32
idf.py build
```

### Arduino ESP32

```bash
ports/arduino/scripts/verify-arduino.sh
```

If `arduino-cli` is missing, local verification can skip compile checks. Release verification should require Arduino CLI.

### MicroPython

```bash
PYTHONPATH=ports/micropython python3 -m unittest discover \
  -s ports/micropython/tests -t .
```

### Compact Wire Fixtures

```bash
python3 -m unittest spec.conformance.test_wire_v2_fixtures
```

## Spec

The [`spec/`](spec/) directory defines contracts that official SDKs and Capture implement:

- [Compact Wire Format](spec/wire-format-v2.md) — active compact binary chunk upload contract.
- [Auto Properties](spec/auto-properties.md) — required SDK-owned properties and lifecycle events.
- [Relay Chunks](spec/relay-chunks.md) — relay frame contract for offline device forwarding.
- [Conformance Fixtures](spec/conformance/) — shared test data for cross-SDK validation.
- [Archived Specs](spec/archive/) — superseded historical material.

## Adding A New SDK

1. Create a device SDK directory under `ports/`, or a companion app relay package under `mobile/`.
2. Implement the compact upload contract from `spec/wire-format-v2.md`.
3. Stamp properties from `spec/auto-properties.md`.
4. Validate against conformance fixtures in `spec/conformance/`.
5. Add build, test, and release-readiness evidence before public production claims.

## License

Apache 2.0
