# Contributing to Honch SDKs

Thanks for your interest in contributing! This repository contains the shared
SDK contract, device SDKs for firmware and embedded Linux, and companion relay
packages for forwarding device-originated payloads.

## Ground Rules

- Be respectful and constructive in all project spaces.
- Keep changes focused. One logical change per pull request.
- Match the surrounding code style. Each port follows the idioms of its
  language and toolchain.
- Don't break the wire contract. Changes to `spec/` are contract changes and
  must keep existing conformance fixtures passing (or update them with a clear
  rationale).

## Development

The SDK behavior is defined once in `core/` and shared across ports. See the
top-level [README](README.md) for the repository layout and per-port build and
test instructions.

Before opening a pull request, please run the relevant build and tests for the
ports you touched:

- **C/POSIX** — `cmake -S ports/posix -B build -DHONCH_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure`
- **MicroPython** — `PYTHONPATH=ports/micropython python3 -m unittest discover -s ports/micropython/tests -t .`
- **Arduino ESP32** — `ports/arduino/scripts/verify-arduino.sh`
- **ESP-IDF** — build the example app under `ports/esp-idf/example`
- **React Native Relay** — `cd mobile/react-native-relay && bun test` (or `npm test`)
- **Wire fixtures** — `python3 -m unittest spec.conformance.test_wire_v2_fixtures`

If you add a new SDK or port, follow the "Adding A New SDK" checklist in the
README and validate against the conformance fixtures in `spec/conformance/`.

## Pull Requests

1. Fork the repository and create a topic branch.
2. Make your change, with tests where applicable.
3. Ensure builds and tests pass for the affected ports.
4. Open a pull request describing the change and the validation you ran.

## Reporting Bugs and Requesting Features

Please open a GitHub issue. For security vulnerabilities, do **not** open a
public issue — follow the process in [SECURITY.md](SECURITY.md).

## License

By contributing, you agree that your contributions will be licensed under the
[Apache License 2.0](LICENSE).
