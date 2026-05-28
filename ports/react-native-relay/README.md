# Honch React Native Relay

React Native relay package for companion apps that receive Honch relay frames
from BLE-only devices, durably assemble queued payloads, and upload them to
Honch capture.

## Production Contract

- Firmware emits relay chunks defined in `spec/relay-chunks.md`.
- The relay validates frame version, reserved byte, payload length, and CRC-16.
- The relay reassembles chunks by source device ID and sequence.
- BLE ACK means the complete compact message has been durably stored by mobile.
- Capture upload sends compact wire-v2 chunk frames to the canonical capture endpoint:

```text
POST /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: <project_api_key>
X-Honch-Stream-Id: <relay_stream_id>
X-Honch-Relay-Id: <mobile_relay_id>
X-Honch-Relay-SDK-Platform: react-native
X-Honch-Relay-SDK-Version: <package_version>
```

- Firmware relay chunks carry compact message bytes. The mobile relay validates
  and reassembles relay frames, durably stores the completed compact message,
  then uploads one or more compact wire-v2 HTTP chunk frames to `/capture`.
- The relay may re-chunk for HTTP, but it must not rewrite the compact message
  body.
- Relay metadata is sent with `X-Honch-Relay-*` headers.
- Retryable upload failures preserve the mobile queue and use exponential
  backoff.

## Setup

```sh
bun install
```

The package depends on `react-native-mmkv` for the production durable store and
`react-native-nitro-modules` for MMKV's native runtime. Both must be autolinked
by the consuming React Native app before native builds will pass.

## Durable Storage

Production mobile apps should use `react-native-mmkv` for relay queue storage:

```ts
import { createMMKV } from "react-native-mmkv";
import { createMmkvRelayStore } from "@honch/react-native-relay";

const relayStore = createMmkvRelayStore(createMMKV({ id: "honch-relay" }));
```

MMKV stores the relay queue under `honch.relay.queue.v1` using the same stable
JSON schema as the local file-backed test adapter. Completed messages and
incomplete assemblies remain pending across app restarts until capture accepts
or permanently rejects them.

## iOS Native Bridge

The iOS module uses CoreBluetooth to scan for the relay service, expose
discovered peripherals to JavaScript, connect to a selected peripheral,
subscribe to the frame notification characteristic, emit `HonchRelayFrame`
events to JavaScript, and write ACK payloads to the ACK characteristic.

Required host app setup:

- Add `NSBluetoothAlwaysUsageDescription` to `Info.plist`.
- Enable the `bluetooth-central` background mode if relay receipt should
  continue while the app is backgrounded.
- Install the package through a React Native iOS host `Podfile` so
  `HonchReactNativeRelay.podspec` links CoreBluetooth and BackgroundTasks.

This package directory does not currently include an `.xcodeproj`,
`.xcworkspace`, or `Podfile`. Native iOS build validation must be run from the
consuming React Native host before production sign-off.

## Android Native Bridge

The Android module uses platform BLE APIs to scan for the relay service, expose
discovered devices to JavaScript, connect to a selected device, discover the
relay frame and ACK characteristics, subscribe to frame notifications, emit
`HonchRelayFrame` events to JavaScript, and write ACK payloads with response.

Required host app setup:

- Request `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` at runtime on Android 12+.
- Request `ACCESS_FINE_LOCATION` where required by target SDK/device BLE scan
  behavior.
- Keep `androidx.work:work-runtime` available for scheduled upload drains.
- Register the package through the consuming React Native Android host.

This package includes an Android library `settings.gradle` for local compile
verification, but it does not include a complete Android host app. Native
Android host validation must still be run from the consuming React Native app
before production sign-off.

## Native Frame Events

The native iOS and Android bridges emit relay notifications as:

```text
HonchRelayFrame
{
  deviceId: "<native peripheral id>",
  frameBase64: "<relay frame bytes>"
}
```

Wire that event source into `createMobileRelay` so native notifications enter
the same durable validation, assembly, ACK, and upload path as manual
`receiveFrame` calls:

```ts
import { NativeEventEmitter, NativeModules } from "react-native";
import { createMobileRelay } from "@honch/react-native-relay";

const nativeModule = NativeModules.HonchReactNativeRelay;
const relay = createMobileRelay({
  durableStore,
  uploaderConfig,
  bleNative,
  schedulerNative,
  frameEvents: new NativeEventEmitter(nativeModule)
});

const subscription = relay.subscribeNativeFrames();
```

Use `relay.discoveredDevices()` after scanning to populate host-app selection
UI before calling `connect(deviceId)` and `subscribeFrames(deviceId)`.

## Upload Draining

`createMobileRelay` exposes three upload controls:

- `drainUploads()` immediately drains pending durable messages once.
- `startUploadScheduler()` asks the native scheduler to run now and drains any
  pending messages when the relay starts.
- `stopUploadScheduler()` cancels native scheduled upload work.

Retryable upload failures keep messages pending and schedule the next native
upload attempt with the canonical relay backoff.

For custom transports, `buildRelayUploadBuffer(config, message)` returns the
canonical compact wire-v2 `ArrayBuffer` for a pending relay message without
marking queue state uploaded. Queue consumption must remain tied to an accepted
capture response.

## Capture E2E

The package includes an offline-capable E2E harness:

```sh
bun run e2e:capture
```

The harness wraps the compact wire-v2 `single-required-context` fixture in
relay frames, feeds those frames through `createMobileRelay().receiveFrame()`,
verifies offline retry preservation, drains to capture, rejects a malformed
frame without ACK, and optionally verifies ClickHouse rows.
For live runs, the harness preflights capture `/health` and ClickHouse `/ping`
so missing services fail before queue-drain assertions.

For full service-backed verification, start capture, worker, Postgres, Redis,
Pub/Sub emulator, and ClickHouse, then run:

```sh
HONCH_CAPTURE_URL=http://127.0.0.1:8001 \
HONCH_PROJECT_KEY=<project_api_key> \
CLICKHOUSE_URL=http://127.0.0.1:8123 \
bun run e2e:capture
```

## Test

```sh
bun run test
```

## Typecheck

```sh
bun run typecheck
```

## Native Verification

```sh
bun run verify:ios:native
bun run verify:android:native
```

`verify:ios:native` performs an iPhoneOS SDK Objective-C syntax check for the
iOS bridge without launching a simulator. `verify:android:native` preflights
the Android SDK location, then builds the Android library with Gradle. Configure
the SDK with `ANDROID_HOME`, `ANDROID_SDK_ROOT`, or
`android/local.properties`.

## Structure

- `src/frame.ts`: relay frame decoder and CRC validation.
- `src/relayQueue.ts`: in-memory and durable queue assembly interfaces.
- `src/durableStore.ts`: durable storage adapter contract and memory test store.
- `src/uploader.ts`: capture delivery and response classification.
- `src/retry.ts`: canonical retry/backoff policy.
- `src/drain.ts`: pending queue upload orchestration.
