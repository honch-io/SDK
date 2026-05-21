# React Native Relay Continuation Plan

**Branch:** `feat/react-native-relay-production`

**Worktree:** `/private/tmp/honch-sdk-react-native-relay`

**Current baseline:** Relay protocol parsing, CRC validation, durable queue
interfaces, retry/backoff, upload draining, native package scaffold, native
module skeletons, TS native bindings, mobile relay orchestration, and example
app harness are committed.

**Fresh verification at checkpoint:**

```text
cd ports/react-native-relay
bun run test      # 9 files, 30 tests passing
bun run typecheck # passing
```

## Remaining Goal

Move from tested relay package foundations to a real hardware path:

```text
firmware packetizer -> BLE/GATT -> mobile relay -> capture -> ClickHouse
```

## Decisions To Lock First

1. **BLE UUIDs**
   - Define Honch service UUID.
   - Define chunk characteristic UUID.
   - Define ACK/control characteristic UUID.

2. **BLE Transfer Shape**
   - Recommended: firmware notifies chunks to phone.
   - Recommended: phone writes ACK/control responses back to firmware.
   - Avoid phone polling unless notification reliability is a practical issue.

3. **ACK Payload**
   - Minimal binary format is preferred.
   - Include at least sequence and status.
   - Statuses should cover durable receipt, CRC failure, unsupported version,
     duplicate mismatch, storage full, and abort/retry.

4. **Firmware Queue Consumption**
   - Current branch assumes BLE ACK means mobile durable receipt.
   - Firmware can consume its queue after durable ACK.
   - Capture upload success remains mobile-owned retry state.

5. **Android Background Drain**
   - Decide whether `WorkManager` wakes JS/headless drain or whether native
     storage/upload becomes required.
   - Recommended first proof: foreground or app-alive JS drain, then add
     WorkManager/headless behavior once the BLE path is proven.

## Task 1: Android BLE Central Implementation

**Files likely touched:**

- `ports/react-native-relay/android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java`
- New Android BLE helper classes under:
  `ports/react-native-relay/android/src/main/java/io/honch/reactnativerelay/`
- `ports/react-native-relay/src/ble.ts`
- `ports/react-native-relay/test/nativeInterfaces.test.ts`
- `spec/relay-chunks.md`

**Work:**

- Add UUID constants for service, chunk characteristic, and ACK characteristic.
- Implement BLE scan filtering for Honch relay service.
- Implement connect/disconnect lifecycle.
- Discover services and characteristics after connection.
- Subscribe to chunk notifications.
- Deliver received frame bytes to JS relay receiver.
- Implement `acknowledgeMessage(deviceId, sequence)` as a write to ACK/control
  characteristic.

**Verification:**

- Keep existing TS tests passing.
- Add Android shape tests for UUID constants/method names.
- Build Android package/example once a full RN app shell exists.

## Task 2: Android Upload Scheduling

**Files likely touched:**

- `HonchRelayUploadWorker.java`
- `HonchReactNativeRelayModule.java`
- `ports/react-native-relay/src/scheduler.ts`
- `ports/react-native-relay/src/mobileRelay.ts`
- Example app files.

**Work:**

- Replace worker no-op with a real upload-drain trigger.
- Add network constraint to scheduled work.
- Ensure scheduled retry uses the delay returned by TS backoff.
- Decide and implement JS/headless handoff or document foreground-only first
  proof.

**Verification:**

- Unit/shape tests for scheduler contract.
- Manual Android run showing retry schedule after simulated 503.

## Task 3: ESP-IDF BLE Relay Peripheral

**Files likely touched:**

- New ESP-IDF relay example or benchtest mode under `ports/esp-idf/`.
- Existing packetizer API in `core/include/honch/core/packetizer.h`.
- Existing packetizer implementation in `core/src/honch_packetizer.c`.
- `spec/relay-chunks.md`.

**Work:**

- Add a BLE GATT server exposing the Honch relay service.
- Use `honch_core_data_available`.
- Use `honch_packetizer_begin`.
- Send chunks from `honch_packetizer_next`.
- Wait for mobile ACK/control response.
- Call `honch_packetizer_confirm` only after durable ACK.
- Call `honch_packetizer_abort` on failed transfer/retry.

**Verification:**

- ESP-IDF build for the relay example.
- Manual monitor logs showing chunk send, ACK receipt, and queue consumption.

## Task 4: Android Hardware E2E

**Flow:**

1. Start capture/worker/ClickHouse stack.
2. Flash ESP32 relay firmware.
3. Launch Android relay example.
4. Firmware queues a unique known event.
5. Firmware packetizer sends BLE chunks.
6. Android relay validates CRC and durably stores the complete CBOR body.
7. Android ACKs durable receipt.
8. Android drains upload to capture.
9. ClickHouse row appears with original device identity and relay metadata.

**Artifacts to keep:**

- Exact flash/build commands.
- Android run command.
- Device logs.
- App logs.
- ClickHouse query.
- Event name used for proof.

## Task 5: iOS Implementation

**Files likely touched:**

- `ports/react-native-relay/ios/HonchReactNativeRelay.h`
- `ports/react-native-relay/ios/HonchReactNativeRelay.m`
- Possibly Swift helper files under `ports/react-native-relay/ios/`.
- Example app iOS config.

**Work:**

- Implement CoreBluetooth central scanning.
- Implement connect/discover/subscribe.
- Implement ACK/control write.
- Add background mode documentation.
- Add BGTaskScheduler upload retry support.

**Constraint:**

iOS background BLE/upload must be documented as best-effort within OS limits,
not a continuously running daemon.

## Task 6: Final E2E Report

**Create:**

- A release evidence document under `local-docs/` or another agreed location.

**Include:**

- Android E2E result.
- iOS E2E result or explicit limitation.
- Capture/ClickHouse evidence.
- Known failure modes.
- Remaining risk acceptances.
- Commands needed to reproduce.

## Recommended Immediate Next Step

Start with **Task 1: Android BLE Central Implementation** plus a small update to
`spec/relay-chunks.md` that pins the UUIDs and ACK/control payload format.

