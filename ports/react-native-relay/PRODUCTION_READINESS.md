# React Native Relay Production Readiness

Status: not production-ready until native host builds and live capture E2E are run.

Verification:

- `bun run typecheck`: passing.
- `bun run test`: passing, 13 files / 51 tests.
- Legacy production contract search:
  `rg -n "POST /batch|/batch|application/cbor" ports/react-native-relay spec`
  returns only negative assertions in uploader tests.
- Offline relay E2E harness: covered by `test/relayE2E.test.ts`.

Known limitations:

- No package-local iOS `.xcodeproj`, `.xcworkspace`, or `Podfile` exists.
- No package-local Android host app, `settings.gradle`, or Gradle wrapper exists.
- iOS and Android native bridge code has static/package-shape coverage only in
  this repository. Host-app compile and hardware validation are still required.
- Service-backed `bun run e2e:capture` with capture, worker, Pub/Sub emulator,
  Postgres, Redis, and ClickHouse running has not been executed in this pass.
- Simulator/device runs were intentionally not performed.

Supported storage adapter:

- Production: `react-native-mmkv` via `createMmkvRelayStore`.
- Test/local harness: JSON file store via `createJsonFileRelayStore`.

Supported native platforms:

- iOS: CoreBluetooth scan/connect/subscribe, `HonchRelayFrame` event emission,
  and ACK characteristic writes are implemented.
- Android: BLE scan/connect/subscribe, `HonchRelayFrame` event emission, ACK
  characteristic writes, and WorkManager upload scheduling are implemented.

Capture contract:

- `POST /capture`
- `Content-Type: application/vnd.honch.chunk`
- `X-Honch-Project-Key`
- `X-Honch-Stream-Id`
- `X-Honch-Relay-Id`
- `X-Honch-Relay-SDK-Platform`
- `X-Honch-Relay-SDK-Version`

Manual hardware validation:

- Pair with a firmware relay peripheral advertising the relay service UUID.
- Verify iOS and Android scan results discover the peripheral.
- Verify connect and frame subscription discover frame and ACK characteristics.
- Verify received frame notifications emit `HonchRelayFrame` and enter
  `subscribeNativeFrames()`.
- Verify malformed frames are rejected without ACK.
- Verify complete messages are ACKed only after durable storage.
- Verify retryable capture failures preserve MMKV queue state across app restart.
- Verify accepted capture responses remove queue state exactly once.
- Run the live capture E2E and confirm ClickHouse rows for relay metadata.
