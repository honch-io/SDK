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

The relay store only requires an MMKV-compatible object with `getString`,
`set`, and `remove`, so host apps on `react-native-mmkv` v2, v3, or v4 can use
their existing MMKV installation.

MMKV relay storage uses per-chunk and per-message records with a small index, so receipt does not rewrite a full queue blob for every chunk. Binary frame, payload, and message bodies are stored as base64 strings instead of JSON number arrays. By default it retains up to 4,096 chunks and 1,024 completed messages with no time-based expiry, dropping the oldest entries once a cap is reached to bound offline growth. Time-based expiry is opt-in via `ttlMs`. Override these limits when the host app has a smaller storage budget:

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

All bundled stores share the same retention model: count caps on chunks and
completed messages with drop-oldest eviction and no time-based expiry by default.
`createMemoryDurableStore` and `createJsonFileRelayStore` accept `maxChunks` and
`maxCompleteMessages` (defaults 4,096 / 1,024); `createMmkvRelayStore` adds the
optional `ttlMs`.

Completed messages and incomplete assemblies remain pending across app restarts until Capture accepts or permanently rejects them, unless they age out or the configured queue bounds require dropping oldest state. Retry attempts and next-attempt timestamps are stored with completed messages, so app restarts do not reset relay backoff or hammer Capture while a message is still inside its retry delay.

## Host-Owned Bluetooth

The relay package does not scan, connect, subscribe, request BLE permissions, or write BLE characteristics for the host app. Production apps should keep their existing Bluetooth stack and pass relay frames into the SDK:

```ts
import { createMobileRelay } from "@honch/react-native-relay";

const relay = createMobileRelay({
  durableStore,
  uploaderConfig,
  schedulerNative
});

hostBle.onRelayFrame(async ({ deviceId, frameBytes }) => {
  await relay.receiveFrame(deviceId, frameBytes, {
    acknowledge: async ({ ackBytes }) => {
      await hostBle.writeRelayAck(deviceId, ackBytes);
    }
  });
});
```

ACK bytes are emitted only after the received relay message has been durably assembled and stored. Malformed frames reject and do not produce ACK bytes. Duplicate completed frames can be ACKed again so firmware retries can settle.

The Honch relay BLE UUIDs and frame format remain defined by `spec/relay-chunks.md`; the host app owns how those characteristics are discovered and wired into its device UX.

## Native Host Requirements

iOS:

- Add the Bluetooth usage strings/background modes required by the host app's own BLE implementation.
- iOS upload scheduling is foreground-only. Call `drainUploads()` from the host
  app foreground lifecycle; `startUploadScheduler()` drains immediately without
  native background scheduling when no scheduler binding is configured.

Android:

- Request the Bluetooth permissions required by the host app's own BLE implementation.
- This package does not merge BLE, location, or notification permissions into
  the host manifest.
- Keep `androidx.work:work-runtime` available for scheduled upload drains.
- Register the package through the consuming React Native Android host.
- Register a headless JS task named `HonchRelayUpload` that calls the same durable queue drain path used by foreground upload drains.

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
