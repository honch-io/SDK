# Relay Envelope

> **Status: Partially implemented.** The canonical C core can packetize queued event messages, and `ports/react-native-relay/` can reassemble and upload relay chunks. A JSON relay envelope is not used on the upload path; relay metadata is currently sent as HTTP headers while the body remains the compact chunk payload.

## Overview

The relay envelope wraps a batch of events from one or more devices, allowing a gateway to forward events on behalf of devices that cannot reach the internet directly (BLE-only, Zigbee, Thread, etc.).

## Reserved Envelope Format

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

The current relay or companion app forwards compact chunk payloads to capture
and stamps relay metadata using transport headers:

```text
X-Honch-Relay-Id: <gateway_device_id>
X-Honch-Relay-SDK-Platform: <platform>
X-Honch-Relay-SDK-Version: <version>
```
