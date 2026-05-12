# Honch SDKs

Product analytics for connected hardware. Pick your platform.

## Available SDKs

| Platform | Status | Path |
|----------|--------|------|
| **ESP-IDF** | v0.1.0 | [`esp-idf/`](esp-idf/) |
| **C/POSIX** | in development | [`c-core/`](c-core/) |

## Spec

The [`spec/`](spec/) directory defines the cross-platform contract that all SDKs implement:

- [Wire Format](spec/wire-format.md) — CBOR batch endpoint, payload shape, retry
- [Auto Properties](spec/auto-properties.md) — required properties, lifecycle events
- [Relay Envelope](spec/relay-envelope.md) — (future) gateway forwarding format
- [Conformance Fixtures](spec/conformance/) — shared test data for cross-SDK validation

The ESP-IDF and C/POSIX SDKs send CBOR ingest payloads to Capture.

## Adding a new SDK

1. Create a directory at the root (e.g. `ios/`, `android/`)
2. Implement the wire format from `spec/wire-format.md`
3. Stamp all properties from `spec/auto-properties.md`
4. Validate against the conformance fixtures in `spec/conformance/`
5. Add a CI workflow in `.github/workflows/<platform>.yml`

## License

Apache 2.0
