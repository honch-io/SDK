# Honch React Native Relay Example

This directory is reserved for the internal mobile E2E harness.

The example app should prove:

- BLE scan/connect to a Honch firmware relay peripheral.
- Relay chunk receipt and CRC validation.
- Durable mobile-side message assembly.
- Manual and scheduled upload draining.
- Capture to ClickHouse verification using a unique event name.

Required app configuration:

- Capture endpoint URL.
- Honch API key or relay-scoped token.
- Relay ID.
- Android BLE permissions and notification permission.
- iOS CoreBluetooth background mode and BGTaskScheduler identifier.

The first production proof should target Android before iOS because Android
WorkManager gives a clearer network-constrained retry path.
