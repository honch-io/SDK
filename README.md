# Honch SDKs

Product analytics for connected hardware. This repository contains the shared
SDK contract plus platform SDKs for embedded and connected-device targets.

## Available SDKs

| Platform | Status | Path |
|----------|--------|------|
| **ESP-IDF** | v0.1.0 | [`ports/esp-idf/`](ports/esp-idf/) |
| **Arduino ESP32** | Planned | [`ports/arduino/`](ports/arduino/) |
| **C/POSIX** | v0.2.0 core-derived | [`ports/posix/`](ports/posix/) |
| **MicroPython** | v0.2.0 C-core wrapper | [`ports/micropython/`](ports/micropython/) |
| **React Native Relay** | In progress | [`ports/react-native-relay/`](ports/react-native-relay/) |

## Repository layout

- [`core/`](core/) — canonical portable C SDK behavior: event semantics,
  compact wire encoding, identity, lifecycle, queue policy, retry
  classification, and packetization.
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

Run the ESP-IDF migration guard from the repository root:

```bash
python3 ports/esp-idf/tests/test_cbor_migration.py
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

- [Wire Format](spec/wire-format.md) — compact binary chunk endpoint,
  frame format, event message grammar, retry behavior, and conformance fixtures
- [Compact Wire Format](spec/wire-format-v2.md) — implementation draft for the
  current compact binary layout
- [Auto Properties](spec/auto-properties.md) — required properties, lifecycle events
- [Relay Envelope](spec/relay-envelope.md) — gateway forwarding and relay metadata
- [Conformance Fixtures](spec/conformance/) — shared test data for cross-SDK validation

## Adding a new SDK

1. Create a directory under `ports/` (e.g. `ports/ios/`, `ports/android/`)
2. Implement the compact wire format from `spec/wire-format.md`
3. Stamp all properties from `spec/auto-properties.md`
4. Validate against the conformance fixtures in `spec/conformance/`
5. Add a CI workflow in `.github/workflows/<platform>.yml`

## License

Apache 2.0
