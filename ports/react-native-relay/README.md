# Honch React Native Relay

Skeleton relay package for React Native companion apps that receive Honch relay
frames from BLE-only devices and upload queued payloads to Honch capture.

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

- `src/frame.ts`: relay frame decoder.
- `src/relayQueue.ts`: queue storage interface for received relay messages.
- `src/uploader.ts`: upload interface for capture delivery.
- `test/frame.test.ts`: frame decoder coverage.
