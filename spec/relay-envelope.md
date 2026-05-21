# Relay Envelope

> **Status: Future backend contract.** React Native relay v0.1 forwards the
> original CBOR batch unchanged to `POST /batch` and sends relay metadata in
> HTTP headers. The envelope below is reserved for a future capture contract
> where relay metadata is part of the CBOR payload itself.

## Overview

The relay envelope wraps a batch of events from one or more devices, allowing a
gateway to forward events on behalf of devices that cannot reach the internet
directly (BLE-only, Zigbee, Thread, etc.).

For v0.1 relay uploads, the companion app does not decode or rewrite the CBOR
batch produced by firmware. It forwards those bytes as `application/cbor` and
adds:

- `X-Honch-Relay-Id`
- `X-Honch-Relay-SDK-Platform`
- `X-Honch-Relay-SDK-Version`

Capture can use those headers to stamp relay metadata without changing the
device-originated event body.

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

<<<<<<< HEAD
The current relay or companion app forwards compact chunk payloads to capture
and stamps relay metadata using transport headers:

```text
X-Honch-Relay-Id: <gateway_device_id>
X-Honch-Relay-SDK-Platform: <platform>
X-Honch-Relay-SDK-Version: <version>
```
=======
The relay or companion app forwards the reassembled CBOR batch to capture and
stamps relay metadata. In v0.1, relay metadata is supplied through headers; a
future envelope implementation must remain backward compatible with raw CBOR
relay uploads or explicitly version the ingest contract.
>>>>>>> 5c3e332 (feat(react-native-relay): add durable relay foundations)
