# React Native Relay Production Readiness

Status: preview. Do not treat the package as production-ready for a customer app until the consuming iOS and Android host apps pass native build, BLE, durable-storage, background scheduling, and live Capture validation.

## Current Evidence

- TypeScript package tests and type checks are expected release gates.
- Native bridge syntax/library checks are useful preflights, but they do not replace host-app archive/build and device validation.
- Offline relay harness coverage proves relay assembly and retry behavior at package level.

## Required Host-App Validation

- Build and archive the consuming iOS host app.
- Build the consuming Android host app.
- Pair with a firmware relay peripheral advertising the relay service.
- Verify iOS and Android scan, connect, frame notification, and ACK writes.
- Verify malformed frames are rejected without ACK.
- Verify complete messages are ACKed only after durable mobile storage.
- Verify retryable Capture failures preserve MMKV queue state across app restart.
- Verify accepted Capture responses remove queue state exactly once.
- Run live Capture validation and confirm relay metadata reaches ingest.

## Supported Storage

- Production: `react-native-mmkv` through `createMmkvRelayStore`.
- Test/local: JSON file store through `createJsonFileRelayStore`.

## Capture Contract

- `POST /capture`
- `Content-Type: application/vnd.honch.chunk`
- `X-Honch-Project-Key`
- `X-Honch-Stream-Id`
- `X-Honch-Relay-Id`
- `X-Honch-Relay-SDK-Platform`
- `X-Honch-Relay-SDK-Version`
