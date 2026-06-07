# Honch React Native Relay Example

This directory contains a host-app example shape for validating the preview
React Native Relay package inside a consuming mobile app.

`relay.ts` wires the package to `NativeModules.HonchReactNativeRelay`, stores
relay queue state in MMKV, and exports the app-owned relay singleton used by
both the foreground UI and Android headless upload task. `App.tsx` subscribes
to native `HonchRelayFrame` events, displays discovered relay devices and
pending mobile relay messages, and exposes manual scan/connect/drain controls.
`index.ts` registers the `HonchRelayUpload` headless task.

The example app should prove:

- BLE scan, discovery listing, and connect to a Honch firmware relay peripheral.
- Relay chunk receipt and CRC validation.
- Durable mobile-side message assembly.
- Manual and scheduled upload draining.
- iOS foreground upload draining from the host app lifecycle.
- Android headless `HonchRelayUpload` task registration for scheduled drains.
- Capture ingestion verification using a unique event name.

Required app configuration:

- Capture endpoint URL.
- Honch project key or relay-scoped token.
- Relay ID.
- iOS `NSBluetoothAlwaysUsageDescription`.
- iOS `bluetooth-central` background mode for background receipt.
- Android `BLUETOOTH_SCAN`.
- Android `BLUETOOTH_CONNECT`.
- The relay package does not merge Android location or notification permissions;
  add host-app permissions only for separate host features.

The package directory is not a complete React Native host app. To run this
example, copy the app into a generated React Native project or add platform
host files (`ios/`, `android/`, `Podfile`, Gradle wrapper), then install this
package through the host app.
