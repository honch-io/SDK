# Honch React Native Relay

Preview React Native relay package for companion apps that receive Honch relay frames from offline devices, durably assemble completed device messages, ACK durable receipt, and upload to Honch Capture.

React Native Relay is not a device analytics SDK. Use it only when firmware cannot upload directly.

## Status

Preview `0.1.0`. Production use requires validation inside the consuming iOS and Android host apps. See [`PRODUCTION_READINESS.md`](PRODUCTION_READINESS.md).

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
bun add @honch/react-native-relay react-native-mmkv
```

The consuming app must register native modules and run its normal iOS/Android dependency installation flow.

## Durable Storage

Production mobile apps should use MMKV:

```ts
import { createMMKV } from "react-native-mmkv";
import { createMmkvRelayStore } from "@honch/react-native-relay";

const relayStore = createMmkvRelayStore(createMMKV({ id: "honch-relay" }));
```

Completed messages and incomplete assemblies remain pending across app restarts until Capture accepts or permanently rejects them.

## Native Host Requirements

iOS:

- Add `NSBluetoothAlwaysUsageDescription`.
- Enable `bluetooth-central` background mode if relay receipt should continue while backgrounded.
- Install through the consuming React Native iOS host `Podfile`.

Android:

- Request `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` at runtime on Android 12+.
- Request `ACCESS_FINE_LOCATION` where required by BLE scan behavior.
- Keep `androidx.work:work-runtime` available for scheduled upload drains.
- Register the package through the consuming React Native Android host.

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

## Test And Verification

```bash
bun run test
bun run typecheck
bun run verify:ios:native
bun run verify:android:native
```

These checks do not replace validation in a consuming host app. Production validation must include BLE behavior, durable storage across app restart, retry preservation, accepted Capture response handling, and live Capture ingestion.
