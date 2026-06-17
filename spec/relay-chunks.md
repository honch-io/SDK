# Relay Chunks

Relay chunks let a device without internet connectivity stream queued Honch data
to a gateway, companion app, or hub. The gateway forwards data to Honch capture
after durable receipt.

## BLE Constants

These UUIDs define the current relay service and characteristics used by Honch
relay implementations:

```text
Service UUID: 484f4e43-482d-5245-4c41-592d53445631
Frame Notify Characteristic UUID: 484f4e43-482d-5245-4c41-592d4652414d
ACK Write Characteristic UUID: 484f4e43-482d-5245-4c41-592d41434b31
```

## Frame Format

| Offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | version | `1` |
| 1 | 1 | source_type | `1` for events |
| 2 | 1 | flags | bit 0 first chunk, bit 1 final chunk |
| 3 | 1 | reserved | `0` |
| 4 | 8 | sequence | uint64 big-endian |
| 12 | 4 | offset | uint32 big-endian |
| 16 | 2 | payload_length | uint16 big-endian |
| 18 | 2 | crc16 | CRC-16 over bytes 0-17 plus payload |
| 20 | n | payload | Compact message bytes |

## Sender Rules

- Send chunks in ascending offset order.
- Keep the queued source message pending until the receiver acknowledges the complete message.
- On failed transfer, abort packetization and retry from offset 0 later.
- Use the smallest MTU-safe payload size selected by the port.

## Receiver Rules

- Reject unsupported versions.
- Reject nonzero reserved bytes.
- Reject frames whose first-chunk flag disagrees with the offset: bit 0 (first)
  is set exactly on the offset-0 frame and clear on every other frame.
- Reassemble by source device ID and sequence.
- Accept duplicate chunks when offset and payload bytes match already stored bytes.
- Acknowledge only after the complete message is durably stored or forwarded
  successfully.
- React Native relay treats BLE ACK as durable mobile receipt. Capture upload
  success is tracked separately by the relay queue and retry scheduler.

## Initial Sources

- `1`: events

Additional source types require a spec update and conformance fixture.

## React Native Relay ACK Policy

The mobile relay acknowledges a firmware message after every chunk for that
message has passed frame validation and the reassembled compact body is durably
stored. The embedded sender may then consume its local queue entry.

If the later capture upload fails with a retryable response, the mobile relay
keeps the complete message pending and schedules retry/backoff without asking
the embedded sender to retransmit. If durable storage fails, the mobile relay
must not acknowledge the message; the sender should abort packetization and
retry from offset `0` later.
