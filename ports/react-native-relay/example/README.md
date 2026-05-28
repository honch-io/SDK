# Honch React Native Relay Example

This directory is reserved for the internal mobile E2E harness.

`App.tsx` wires the package to `NativeModules.HonchReactNativeRelay`, subscribes
to native `HonchRelayFrame` events, stores relay queue state in MMKV, displays
discovered relay devices and pending mobile relay messages, and exposes manual
scan/connect/drain controls.

The example app should prove:

- BLE scan, discovery listing, and connect to a Honch firmware relay peripheral.
- Relay chunk receipt and CRC validation.
- Durable mobile-side message assembly.
- Manual and scheduled upload draining.
- Capture to ClickHouse verification using a unique event name.

Required app configuration:

- Capture endpoint URL.
- Honch project key or relay-scoped token.
- Relay ID.
- iOS `NSBluetoothAlwaysUsageDescription`.
- iOS `bluetooth-central` background mode for background receipt.
- Android `BLUETOOTH_SCAN`.
- Android `BLUETOOTH_CONNECT`.
- Android `ACCESS_FINE_LOCATION` when required by target SDK/device behavior.
- Android notification permission if upload status notifications are added by
  the host app.

The package directory is not a complete React Native host app. To run this
example, copy the app into a generated React Native project or add platform
host files (`ios/`, `android/`, `Podfile`, Gradle wrapper), then install this
package through the host app.
