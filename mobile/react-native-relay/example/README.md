# Honch React Native Relay Example

This directory contains a host-app example shape for validating the preview
React Native Relay package inside a consuming mobile app.

`relay.ts` wires the package to `NativeModules.HonchReactNativeRelay`, stores
relay queue state in MMKV, and exports the app-owned relay singleton used by
both the foreground UI and Android headless upload task. `App.tsx` shows the
host-owned handoff shape: the host app's BLE stack calls `relay.receiveFrame`
with notification bytes and writes the returned ACK bytes itself.
`index.ts` registers the `HonchRelayUpload` headless task.

The example app should prove:

- Host-owned BLE scan, discovery, connect, notify subscription, and ACK writes.
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
- Bluetooth usage strings, background modes, and runtime permissions required by
  the host app's own BLE implementation.
- The relay package does not merge Android BLE, location, or notification
  permissions; add host-app permissions only for host-owned BLE or separate
  host features.

The package directory is not a complete React Native host app. To run this
example, copy the app into a generated React Native project or add platform
host files (`ios/`, `android/`, `Podfile`, Gradle wrapper), then install this
package through the host app.
