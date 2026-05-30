# Honch SDKs

Product analytics for connected hardware. This repository contains the shared
SDK contract plus platform SDKs for embedded and connected-device targets.

## Available SDKs

| Platform | Status | Path |
|----------|--------|------|
| **ESP-IDF** | v0.1.0 | [`ports/esp-idf/`](ports/esp-idf/) |
| **Arduino ESP32** | In progress | [`ports/arduino/`](ports/arduino/) |
| **C/POSIX** | v0.2.0 | [`ports/posix/`](ports/posix/) |
| **MicroPython** | v0.2.0 | [`ports/micropython/`](ports/micropython/) |
| **React Native Relay** | In progress | [`ports/react-native-relay/`](ports/react-native-relay/) |

## Repository layout

- [`core/`](core/) — canonical portable C SDK behavior: typed event
  properties, HQR1 queue records, compact wire encoding, identity, lifecycle,
  queue policy, retry classification, and packetization.
- [`ports/arduino/`](ports/arduino/) — Arduino ESP32 SDK wrapper around the
  canonical core.
- [`ports/posix/`](ports/posix/) — C/POSIX SDK and local development port for
  the canonical core.
- [`ports/esp-idf/`](ports/esp-idf/) — ESP-IDF component, example app,
  RAM-first/NVS-configurable storage adapter, and ESP-focused tests.
- [`ports/micropython/`](ports/micropython/) — MicroPython SDK validated against
  the same wire-format and lifecycle contract.
- [`ports/react-native-relay/`](ports/react-native-relay/) — relay package for
  forwarding device-originated SDK payloads through mobile apps.
- [`spec/`](spec/) — shared cross-platform SDK contract and conformance data.

## Build and test

### Production release E2E

Use the release campaign runner before release candidates. It runs the
automatable production gates, writes per-step logs, and generates a readiness
report under `/private/tmp/honch-production-e2e-*`.

```bash
python3 tools/release_e2e.py --profile release --continue-on-fail
```

Profiles:

- `smoke`: fastest deterministic host gate.
- `host`: deterministic host checks only, no local services or hardware.
- `services`: local sandbox/capture E2E checks only.
- `toolchains`: ESP-IDF, Arduino CLI, and PlatformIO compile gates.
- `hardware`: configured board preflights.
- `release`: host + services + toolchains.
- `full`: release + hardware.

Missing required services, toolchains, or hardware inputs are reported as
`BLOCKED`, not passed. Start the local sandbox with `./honch --plain sandbox`
before the `services`, `release`, or `full` profiles.

### C/POSIX

```bash
cd ports/posix
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To build the POSIX benchmark target:

```bash
cd ports/posix
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DHONCH_BUILD_TESTS=OFF -DHONCH_BUILD_EXAMPLES=OFF \
  -DHONCH_BUILD_BENCHMARKS=ON
cmake --build build-bench --target honch_posix_bench
```

### ESP-IDF

Activate your ESP-IDF environment first, then build the example app:

```bash
cd ports/esp-idf/example
idf.py set-target esp32
idf.py build
```

Run the ESP-IDF SDK contract guard from the repository root:

```bash
python3 ports/esp-idf/tests/test_sdk_contract.py
```

### Arduino ESP32

Run the Arduino host wrapper test and, when `arduino-cli` is installed, compile
the ESP32 examples:

```bash
ports/arduino/scripts/verify-arduino.sh
```

### Compact wire fixtures

```bash
python3 tools/generate_wire_v2_fixtures.py
python3 -m unittest spec.conformance.test_wire_v2_fixtures
```

### MicroPython

```bash
PYTHONPATH=ports/micropython python3 -m unittest discover \
  -s ports/micropython/tests -t .
```

## Spec

The [`spec/`](spec/) directory defines the cross-platform contract that all SDKs implement:

- [Compact Wire Format](spec/wire-format-v2.md) — active compact binary chunk
  endpoint, frame format, event message grammar, retry behavior, and
  conformance fixtures
- [Auto Properties](spec/auto-properties.md) — required properties, lifecycle events
- [Conformance Fixtures](spec/conformance/) — shared test data for cross-SDK validation
- [Archived Specs](spec/archive/) — superseded wire-format overview and
  historical relay envelope notes

## Adding a new SDK

1. Create a directory under `ports/` (e.g. `ports/ios/`, `ports/android/`)
2. Implement the compact wire format from `spec/wire-format-v2.md`
3. Stamp all properties from `spec/auto-properties.md`
4. Validate against the conformance fixtures in `spec/conformance/`
5. Add a CI workflow in `.github/workflows/<platform>.yml`

## License

Apache 2.0
