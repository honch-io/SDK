# Honch Swift Relay

Native Swift companion-app relay for BLE-only Honch devices.

This package is a relay/uploader, not an application analytics SDK. It does not
provide `track`, `identify`, or app-event instrumentation APIs. A host app moves
Honch relay frame bytes from a paired device into `HonchRelay`; the relay
validates, reassembles, durably stores, acknowledges, and uploads those
device-originated compact messages.

## BLE Topology

The current Honch relay service and characteristics are defined in
`../../spec/relay-chunks.md`:

```text
Service UUID: 484f4e43-482d-5245-4c41-592d53445631
Frame Notify Characteristic UUID: 484f4e43-482d-5245-4c41-592d4652414d
ACK Write Characteristic UUID: 484f4e43-482d-5245-4c41-592d41434b31
```

The host app owns CoreBluetooth scanning, connection, notification subscription,
and ACK characteristic writes. Pass each complete frame notification value into
`receiveFrame(deviceId:frameBytes:)`. Write returned `ackBytes` to the ACK
characteristic only when the receipt is complete.

## Durable ACK Contract

`HonchRelay` validates relay frame version, source type, flags, reserved byte,
payload length, and CRC before accepting a chunk. It reassembles chunks by
`deviceId + sequence` and accepts exact duplicate chunks so firmware retries can
settle.

The relay returns ACK bytes only after the complete compact message body has
been stored durably. ACK bytes are one version byte, `0x01`, followed by the
uint64 relay sequence in big-endian order.

## Upload Contract

Completed relay messages remain pending until capture accepts or permanently
rejects them. Uploads use:

```text
POST /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: <project capture key>
X-Honch-Stream-Id: <stream id>
X-Honch-Relay-Id: <relay id>
X-Honch-Relay-SDK-Platform: <platform>
X-Honch-Relay-SDK-Version: <version>
```

The relay wraps the preserved compact message body in a single compact wire-v2
HTTP chunk frame. It does not decode, rewrite, or restamp the compact message
body.

Response handling matches the React Native relay:

- `204`: consume the pending message.
- `400`, `401`, `404`: drop as permanent rejection.
- network errors and all other statuses: keep pending and retry.

## Basic Use

```swift
import Foundation
import HonchSwiftRelay

let storeURL = FileManager.default
    .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    .appendingPathComponent("HonchRelay", isDirectory: true)

let relay = HonchRelay(
    store: FileRelayStore(rootDirectory: storeURL),
    config: HonchRelayConfig(
        endpointURL: URL(string: "https://capture.honch.io")!,
        projectKey: "<project capture key>",
        relayId: "ios-phone-1",
        relaySdkVersion: "0.1.0",
        streamId: { "relay-\($0.deviceId)" },
        messageId: { UInt64($0.sequence) ?? 0 }
    )
)

let receipt = try await relay.receiveFrame(
    deviceId: peripheral.identifier.uuidString,
    frameBytes: frameData
)

if receipt.complete, let ackBytes = receipt.ackBytes {
    // Write ackBytes to the Honch ACK characteristic.
}

try await relay.drainUploads()
```

For background upload, provide a `RelayScheduling` adapter from the host app and
call `startUploadScheduler()`. The core package does not request background
modes or register BGTaskScheduler identifiers for the host app.

## Verification

The local toolchain used while creating this package did not provide XCTest, so
the package includes an executable contract harness:

```bash
swift run HonchSwiftRelayContractTests
```

If running with a full Xcode toolchain, host apps may add their own XCTest
coverage around CoreBluetooth integration and background scheduling adapters.
