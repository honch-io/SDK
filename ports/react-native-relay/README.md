# Honch React Native Relay

React Native relay package for companion apps that receive Honch relay frames
from BLE-only devices, durably assemble queued payloads, and upload them to
Honch capture.

## v0.1 Contract

- Firmware emits relay chunks defined in `spec/relay-chunks.md`.
- The relay validates frame version, reserved byte, payload length, and CRC-16.
- The relay reassembles chunks by source device ID and sequence.
- BLE ACK means the complete CBOR message has been durably stored by mobile.
- Capture upload forwards the original CBOR bytes unchanged to `POST /batch`.
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
