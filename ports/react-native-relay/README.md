# Honch React Native Relay

React Native relay package for companion apps that receive Honch relay frames
from BLE-only devices, durably assemble queued payloads, and upload them to
Honch capture.

## Production Contract

- Firmware emits relay chunks defined in `spec/relay-chunks.md`.
- The relay validates frame version, reserved byte, payload length, and CRC-16.
- The relay reassembles chunks by source device ID and sequence.
- BLE ACK means the complete compact message has been durably stored by mobile.
- Capture upload sends compact wire-v2 chunk frames to the canonical capture endpoint:

```text
POST /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: <project_api_key>
X-Honch-Stream-Id: <relay_stream_id>
X-Honch-Relay-Id: <mobile_relay_id>
X-Honch-Relay-SDK-Platform: react-native
X-Honch-Relay-SDK-Version: <package_version>
```

- Firmware relay chunks carry compact message bytes. The mobile relay validates
  and reassembles relay frames, durably stores the completed compact message,
  then uploads one or more compact wire-v2 HTTP chunk frames to `/capture`.
- The relay may re-chunk for HTTP, but it must not rewrite the compact message
  body.
- Relay metadata is sent with `X-Honch-Relay-*` headers.
- Retryable upload failures preserve the mobile queue and use exponential
  backoff.

## Setup

```sh
bun install
```

## Test

```sh
bun run test
```

## Typecheck

```sh
bun run typecheck
```

## Structure

- `src/frame.ts`: relay frame decoder and CRC validation.
- `src/relayQueue.ts`: in-memory and durable queue assembly interfaces.
- `src/durableStore.ts`: durable storage adapter contract and memory test store.
- `src/uploader.ts`: capture delivery and response classification.
- `src/retry.ts`: canonical retry/backoff policy.
- `src/drain.ts`: pending queue upload orchestration.
