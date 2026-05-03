# Relay Envelope

> **Status: Not yet implemented.** This spec is reserved for the relay/gateway use case where a constrained device sends events to a local gateway (phone, hub) which then forwards to capture.

## Overview

The relay envelope wraps a batch of events from one or more devices, allowing a gateway to forward events on behalf of devices that cannot reach the internet directly (BLE-only, Zigbee, Thread, etc.).

## Envelope Format

```json
{
  "token": "<api_key>",
  "relay": {
    "relay_id": "<gateway_device_id>",
    "relay_sdk_platform": "<platform>",
    "relay_sdk_version": "<version>"
  },
  "batch": [
    {
      "event": "...",
      "distinct_id": "...",
      "timestamp": "...",
      "properties": { ... }
    }
  ]
}
```

The `relay` object identifies the gateway. Events in the batch may come from different `distinct_id` values (different devices).

## SDK Surface (future)

```c
// Drain queued events into a buffer for relay transport
honch_err_t honch_drain_to_buffer(uint8_t *buf, size_t buf_size, size_t *out_len);
```

## Open Questions

- CBOR encoding for constrained transports?
- Maximum relay batch size?
- Authentication: does the relay use its own API key or the device's?
