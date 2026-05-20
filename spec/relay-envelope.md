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

## SDK Surface

```c
bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(honch_packetizer_t *packetizer, uint8_t *buffer, size_t buffer_size, size_t *out_size, bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);
```

The relay or companion app forwards the reassembled CBOR batch to capture and stamps relay metadata.
