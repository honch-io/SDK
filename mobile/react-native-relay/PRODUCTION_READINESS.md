# React Native Relay Production Readiness

Status: release candidate. Do not treat the package as production-ready for a customer app until the consuming iOS and Android host apps pass native build, BLE, durable-storage, background scheduling, and live Capture validation.

## Current Evidence

- TypeScript package tests and type checks are expected release gates.
- Native bridge syntax/library checks are useful preflights, but they do not replace host-app archive/build and device validation.
- Offline relay harness coverage proves relay assembly and retry behavior at package level.
- All durable stores (MMKV, JSON file, in-memory) are bounded by count caps with drop-oldest; time-based TTL expiry is opt-in (MMKV `ttlMs`) and off by default.
- MMKV package tests cover per-record storage, per-sequence chunk indexes, legacy queue migration, TTL expiry, completed-message caps, persisted retry metadata, and retry scheduling with `Retry-After`.

Latest local package evidence from June 17, 2026:

- `bun run test`: 12 files, 90 tests passed.
- `bun run typecheck`: passed.
- `bun run verify:ios:native`: passed the Objective-C syntax preflight against the active Xcode / iPhoneOS SDK.
- `bun run verify:android:native`: passed Android library `assembleDebug` when Gradle was allowed to use its normal user cache.
- `bun run e2e:capture`: blocked locally (no Capture service reachable at `http://127.0.0.1:8001`). The relay upload path itself was validated separately by a live Capture→ClickHouse end-to-end run on Citadel: a re-chunked relay message drained to empty and its event reached ingest.

## Required Host-App Validation

- Build and archive the consuming iOS host app.
- Build the consuming Android host app.
- Build both platforms in a temporary React Native validation host if no customer host app is available.
- Pair with a firmware relay peripheral advertising the relay service.
- Verify iOS and Android scan, connect, frame notification, and ACK writes.
- Verify malformed frames are rejected without ACK.
- Verify complete messages are ACKed only after durable mobile storage.
- Verify retryable Capture failures preserve MMKV queue state across app restart.
- Verify retry attempt metadata and next-attempt timestamps survive app restart and do not reset backoff.
- Verify `Retry-After` schedules the next drain at the server-requested delay.
- Verify the store's count caps (and optional MMKV TTL) match the host app's offline budget.
- Verify Android `HonchRelayUpload` headless JS task runs from WorkManager in foreground, background, and cold-start conditions.
- Verify accepted Capture responses remove queue state exactly once.
- Run live Capture validation and confirm relay metadata reaches ingest.

## Remaining Signoff Inputs

- A live `HONCH_CAPTURE_URL`, `HONCH_PROJECT_KEY`, and optional `CLICKHOUSE_URL`
  for `bun run e2e:capture`.
- A real iOS and Android host app or temporary React Native validation host.
- A firmware relay peripheral using the UUIDs from `../../spec/relay-chunks.md`.

## Supported Storage

- Preview production-like validation: `react-native-mmkv` through `createMmkvRelayStore`.
- Test/local: JSON file store through `createJsonFileRelayStore`.

## Capture Contract

- `POST /capture`
- `Content-Type: application/vnd.honch.chunk`
- `X-Honch-Project-Key`
- `X-Honch-Stream-Id`
- `X-Honch-Relay-Id`
- `X-Honch-Relay-SDK-Platform`
- `X-Honch-Relay-SDK-Version`
