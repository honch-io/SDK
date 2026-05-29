# Relay Envelope

> **Status: Historical design note.** Production React Native relay uploads use
> the compact wire-v2 `POST /capture` contract. This envelope is not an active
> production relay contract; it is retained only as background for any future
> payload-level relay metadata design.

## Overview

The relay envelope was a proposed wrapper for a batch of events from one or
more devices, allowing a gateway to forward events on behalf of devices that
cannot reach the internet directly (BLE-only, Zigbee, Thread, etc.).

Production relay uploads do not use this envelope. Firmware relay chunks carry
compact message bytes. The companion app validates and reassembles relay
frames, durably stores the completed compact message, then uploads one or more
compact wire-v2 HTTP chunk frames to `/capture` with:

- `X-Honch-Project-Key`
- `X-Honch-Stream-Id`
- `X-Honch-Relay-Id`
- `X-Honch-Relay-SDK-Platform`
- `X-Honch-Relay-SDK-Version`

The relay may re-chunk for HTTP, but it must not rewrite the compact message
body. Capture can use the headers to stamp relay metadata without changing the
device-originated event body.

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

The `relay` object identifies the gateway. Events in the batch may come from
different `distinct_id` values (different devices).

## SDK Surface

```c
bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(honch_packetizer_t *packetizer, uint8_t *buffer, size_t buffer_size, size_t *out_size, bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);
```

The relay or companion app forwards the reassembled compact message to capture
and stamps relay metadata through headers. A future envelope implementation
must be explicitly versioned and must not replace the production compact
wire-v2 relay ingest contract without a separate migration plan.
