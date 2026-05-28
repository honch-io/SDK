# React Native Relay Production Readiness

Status: not production-ready until native host builds and live capture E2E are run.

Verification:

- `bun run typecheck`: passing.
- `bun run test`: passing, 13 files / 56 tests.
- `DEVELOPER_DIR=/Volumes/X9\ Pro/Applications/Xcode.app/Contents/Developer bun run verify:ios:native`:
  passing. This runs an iPhoneOS SDK Objective-C syntax check for
  `ios/HonchReactNativeRelay.m`; it does not boot or run a simulator.
- `bun run verify:android:native`: passing. This builds the Android relay
  library with Gradle using the configured local Android SDK.
- Legacy production contract search:
  `rg -n "POST /batch|/batch|application/cbor" ports/react-native-relay spec`
  returns only negative assertions in uploader tests.
- Offline relay E2E harness: covered by `test/relayE2E.test.ts`.

Known limitations:

- No package-local iOS `.xcodeproj`, `.xcworkspace`, or `Podfile` exists.
- No package-local Android host app or Gradle wrapper exists; Android library
  build verification uses the React Native Gradle plugin wrapper from
  `node_modules`.
- iOS native bridge code has static/package-shape coverage plus an iPhoneOS SDK
  syntax check. A host-app archive/build is still required.
- Android native bridge code has static/package-shape coverage plus Android
  library compile verification.
- Service-backed `bun run e2e:capture` with capture, worker, Pub/Sub emulator,
  Postgres, Redis, and ClickHouse running has not been executed in this pass.
- Simulator/device runs were intentionally not performed.

Supported storage adapter:

- Production: `react-native-mmkv` via `createMmkvRelayStore`.
- Test/local harness: JSON file store via `createJsonFileRelayStore`.

Supported native platforms:

- iOS: CoreBluetooth scan/discovery/connect/subscribe, `HonchRelayFrame` event
  emission, and ACK characteristic writes are implemented.
- Android: BLE scan/discovery/connect/subscribe, `HonchRelayFrame` event
  emission, ACK characteristic writes, and WorkManager upload scheduling are
  implemented.

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
