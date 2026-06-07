# Honch React Native Relay

Release-candidate React Native relay package for companion apps that receive Honch relay frames from offline devices, durably assemble completed device messages, ACK durable receipt, and upload to Honch Capture.

React Native Relay is not a device analytics SDK. Use it only when firmware cannot upload directly.

## Status

Release-candidate `0.1.0`. Production use still requires validation inside the consuming iOS and Android host apps. See [`PRODUCTION_READINESS.md`](PRODUCTION_READINESS.md).

## Capture Contract

Relay uploads use the canonical Capture endpoint:

```text
POST /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: <project_api_key>
X-Honch-Stream-Id: <relay_stream_id>
X-Honch-Relay-Id: <mobile_relay_id>
X-Honch-Relay-SDK-Platform: react-native
X-Honch-Relay-SDK-Version: <package_version>
```

Firmware relay chunks carry compact message bytes. The mobile relay validates and reassembles relay frames, durably stores the completed compact message, then uploads one or more HTTP chunk frames. It may re-chunk for HTTP but must not rewrite the compact device message body.

## Setup

Install this package and a production durable store into a React Native host app:

```bash
bun add @honch/react-native-relay
```

The consuming app must register native modules and run its normal iOS/Android dependency installation flow.

## Durable Storage

Production mobile apps should use MMKV. It is an optional peer dependency, so
install it only when using `createMmkvRelayStore`:

```bash
bun add react-native-mmkv react-native-nitro-modules
```

```ts
import { createMMKV } from "react-native-mmkv";
import { createMmkvRelayStore } from "@honch/react-native-relay";

const relayStore = createMmkvRelayStore(createMMKV({ id: "honch-relay" }));
```

MMKV relay storage uses per-chunk and per-message records with a small index, so receipt does not rewrite a full queue blob for every chunk. Binary frame, payload, and message bodies are stored as base64 strings instead of JSON number arrays. By default it retains up to 4,096 chunks and 1,024 completed messages for seven days, then drops the oldest or expired entries to bound offline growth. Override these limits when the host app has a smaller storage budget:

```ts
const relayStore = createMmkvRelayStore(createMMKV({ id: "honch-relay" }), {
  keyPrefix: "com.example.honch.relay",
  maxChunks: 1024,
  maxCompleteMessages: 256,
  ttlMs: 3 * 24 * 60 * 60 * 1000
});
```

Use `keyPrefix` when sharing an MMKV instance with host app data. The default
prefix is `honch.relay`.

Completed messages and incomplete assemblies remain pending across app restarts until Capture accepts or permanently rejects them, unless they age out or the configured queue bounds require dropping oldest state. Retry attempts and next-attempt timestamps are stored with completed messages, so app restarts do not reset relay backoff or hammer Capture while a message is still inside its retry delay.

## Native Host Requirements

iOS:

- Add `NSBluetoothAlwaysUsageDescription`.
- Enable `bluetooth-central` background mode if relay receipt should continue while backgrounded.
- Install through the consuming React Native iOS host `Podfile`.
- iOS upload scheduling is foreground-only. Call `drainUploads()` from the host
  app foreground lifecycle; `startUploadScheduler()` is only backed by native
  scheduled work on Android.

Android:

- Request `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` at runtime on Android 12+.
- The package marks `BLUETOOTH_SCAN` with `neverForLocation` and does not merge
  `ACCESS_FINE_LOCATION` or notification permissions into the host manifest.
- `requestRelayAndroidPermissions()` requests only Android 12+ BLE permissions
  by default. For a pre-Android-12 host that intentionally needs legacy
  location-backed scans, call it with `requestLegacyLocation: true` and the
  host app's API-level context.
- Keep `androidx.work:work-runtime` available for scheduled upload drains.
- Register the package through the consuming React Native Android host.
- Register a headless JS task named `HonchRelayUpload` that calls the same durable queue drain path used by foreground upload drains.

Native scans auto-stop after an idle timeout to avoid continuous BLE scan drain.
Call `startScan()` again when the host app is ready to discover relay devices.

## Native Frame Events

Native iOS and Android bridges emit:

```text
HonchRelayFrame
{
  deviceId: "<native peripheral id>",
  frameBase64: "<relay frame bytes>"
}
```

Wire this event source into `createMobileRelay` so native notifications enter the same durable validation, assembly, ACK, and upload path as manual `receiveFrame` calls.

```ts
import { NativeEventEmitter, NativeModules } from "react-native";
import { createMobileRelay } from "@honch/react-native-relay";

const nativeModule = NativeModules.HonchReactNativeRelay;
const relay = createMobileRelay({
  durableStore,
  uploaderConfig,
  bleNative,
  schedulerNative,
  frameEvents: new NativeEventEmitter(nativeModule),
});

const subscription = relay.subscribeNativeFrames();
```

## Upload Draining

`createMobileRelay` exposes:

- `drainUploads()` to drain pending messages once;
- `startUploadScheduler()` to start scheduled drains;
- `stopUploadScheduler()` to cancel scheduled drain work.

Retryable upload failures keep messages pending and schedule the next native upload attempt with relay backoff.
If Capture returns `Retry-After` on a retryable response, that delay takes precedence over the local exponential backoff.
Upload retry timing and attempt state stay in the JavaScript relay drain path and durable store. Android WorkManager only retries failures to launch the headless task, and the headless task timeout is capped at 10 seconds to bound wake-lock hold time.

Android scheduled drains start the `HonchRelayUpload` headless JS task from WorkManager. Register the task in the host app entrypoint and call the app-owned relay singleton:

```ts
import { AppRegistry } from "react-native";

AppRegistry.registerHeadlessTask("HonchRelayUpload", () => async () => {
  await relay.drainUploads();
});
```

## Test And Verification

```bash
bun run test
bun run typecheck
bun run e2e:capture
bun run verify:ios:native
bun run verify:android:native
```

These checks do not replace validation in a consuming host app. Production validation must include BLE behavior, durable storage across app restart, retry preservation, accepted Capture response handling, and live Capture ingestion.
