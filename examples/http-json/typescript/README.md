# Honch JSON ingestion — TypeScript reference client (companion-app relay)

A **reference implementation** of a client for the Honch JSON ingestion HTTP API,
modeling the common hardware topology:

```
device (firmware)  ──BLE/serial──▶  your mobile/companion app  ──HTTPS──▶  Honch
                                     (this client runs HERE)
```

It exists to be **read and ported** into your own app — it is *not* a published
package. `honchClient.ts` has **zero runtime dependencies** (uses the global
`fetch`), is heavily commented, and favors correctness/readability over cleverness.

Two things shape it, because the events come from a device and the phone is the
unreliable hop:

- The **`context` describes the paired device**, not the phone (`$device_id`,
  `$device_model`, `$firmware_version`). One client relays one device.
- It preserves each event's **real device time** and supports **durable
  persistence** so the pending queue survives the app being backgrounded/killed.

## What it does

- Buffers events in a queue (optionally durable — see persistence below).
- Flushes to `POST {host}/capture` by size (`flushEventThreshold`), on an
  interval (cleared on `close()`), or explicitly via `flush()` / `close()`.
- Sends the wire body `{ context, events }` with the `X-Honch-Project-Key` auth
  header. Context is sent once; the **server** promotes context keys into each
  event — the client does not pre-merge or inject defaults.
- As a relay, you pass the **device's** event time to `track(event, props, tsMs)`
  so funnels/timelines stay correct even when the phone was offline.
- `identify(userId, set?, setOnce?)` emits a `$identify` event carrying
  `$anon_distinct_id` (the previous device id) — this is what **merges** the
  anonymous device person into the user. `setPersonProperties()` emits `$set`.
- Retries on `429`, `5xx`, and network errors with exponential backoff
  (initial 1s, max 5min, ±25% jitter); **drops** on other `4xx` (permanent).
  Request timeout via `AbortController`.
- `validate()` calls `POST {host}/capture/validate` (a dry-run that stores
  nothing) so you can preview how the server will expand your batch.

## Durable persistence (recommended on mobile)

By default the queue is in-memory. To survive app restarts, wire two hooks to
your storage (MMKV, AsyncStorage, SQLite — anything):

```ts
const client = new HonchClient({
  apiKey, deviceId, deviceModel, firmwareVersion,
  // Rehydrate events saved before the last shutdown:
  initialQueue: JSON.parse(storage.getString("honch.pending") ?? "[]"),
  // Persist the pending queue on every change (queued / sent / dropped):
  onQueueChange: (pending) => storage.set("honch.pending", JSON.stringify(pending)),
});
```

A chunk stays in the persisted queue for its whole send + retry sequence and is
removed only once the server accepts it (or it is permanently dropped), so a
crash mid-send — even during a long backoff — resumes on the next start
(at-least-once delivery).

## API contract (summary)

- Endpoint: `POST {host}/capture`, `Content-Type: application/json`,
  default host `https://i.honch.io`.
- Auth header: `X-Honch-Project-Key: <project_api_key>`.
- Required context: `distinct_id`, `$device_id`, `$device_model`,
  `$firmware_version`, `$sdk_platform`, `$sdk_version`.
  Optional: `$environment` (server default `production`), `$session_id`.
- Each event: required non-empty `event`, optional `timestamp`
  (epoch-ms number or RFC3339 string), optional `properties`. Max 500 events/request.
- Success `200` → `{ status: "ok", accepted: N, rejected: M, errors: [...] }`
  (partial acceptance — `rejected`/`errors` describe events that were permanently
  dropped). Whole-request failures → `4xx` with `{ errors: [ { code, message, field } ] }`.

See the canonical wire shapes in `sdks/spec/conformance/json/`.

## Run the tests

```bash
node --test
```

The tests prove `buildBatch(...)` reproduces every valid fixture's `request.body`
(the encoder matches the documented wire shape), that the queue persists and
rehydrates, and that `identify()` emits a `$identify` with `$anon_distinct_id`.
Verified with **Node 25**, which strips TypeScript types natively (no build
step). On older Node, run through a TS loader, e.g. `npx tsx --test` or
`node --experimental-strip-types --test` (Node ≥ 22.6).

## Strict typecheck

```bash
npm install        # dev-only: typescript + @types/node
npm run typecheck  # tsc --noEmit (strict)
```

The dev dependencies are only for typechecking — the reference client itself has
no runtime dependencies.

## Usage

```ts
import { HonchClient } from "./honchClient.ts";

const client = new HonchClient({
  apiKey: "phk_live_xxx",
  deviceId: "device-1", deviceModel: "model-x", firmwareVersion: "1.0.0",
  sdkPlatform: "react-native", // your relay platform
  environment: "production",
});

client.deviceBoot("power_on");                     // anonymous: distinct_id == device-1
await client.identify("user-1", { plan: "pro" });  // emits $identify ($anon_distinct_id
                                                   // = device-1) → merges into "user-1"
client.sessionStart("recording");
client.track("frame_captured", { index: 1 }, deviceEventTimeMs); // pass the device's time
client.sessionEnd();
await client.close();                              // flush remaining events, stop the timer
```
