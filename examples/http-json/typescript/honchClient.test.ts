/**
 * Conformance test for the TypeScript reference client's request encoder.
 *
 * Proves that `buildBatch(context, events)` reproduces the canonical wire body
 * documented in the shared JSON conformance fixtures. If this passes, the
 * reference encoder matches the on-the-wire shape the server expects.
 *
 * We only assert against fixtures whose `expected_ingest_status` is 200 — those
 * carry a canonical, accepted `request.body`. The `invalid-*` fixtures exist to
 * exercise *server-side* validation, not client encoding, so they are skipped
 * here; a separate assertion just confirms they exist (they drive server-side
 * conformance testing).
 *
 * Run with the built-in test runner:  node --test
 * (Node strips the TS types natively; see package.json's "test" script.)
 */

import { strict as assert } from "node:assert";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

import {
  buildBatch,
  HonchClient,
  type Batch,
  type Context,
  type HonchEvent,
} from "./honchClient.ts";

// Fixtures live at sdks/spec/conformance/json/ relative to this file:
//   .../examples/http-json/typescript/honchClient.test.ts
//   .../spec/conformance/json/*.json
const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURE_DIR = join(HERE, "..", "..", "..", "spec", "conformance", "json");

interface Fixture {
  name: string;
  expected_ingest_status: number;
  expected_error_codes?: string[];
  request: { body: Batch };
}

function loadFixtures(): Fixture[] {
  return readdirSync(FIXTURE_DIR)
    .filter((f) => f.endsWith(".json"))
    .sort()
    .map((f) => JSON.parse(readFileSync(join(FIXTURE_DIR, f), "utf-8")) as Fixture);
}

const fixtures = loadFixtures();

test("fixtures are present", () => {
  assert.ok(fixtures.length > 0, `no fixtures found in ${FIXTURE_DIR}`);
});

test("buildBatch reproduces request.body for every valid (200) fixture", () => {
  let checked = 0;
  for (const fx of fixtures) {
    if (fx.expected_ingest_status !== 200) continue; // invalid-* test the server
    const body = fx.request.body;
    const produced = buildBatch(body.context as Context, body.events as HonchEvent[]);
    assert.deepEqual(
      produced,
      body,
      `buildBatch did not reproduce wire body for ${fx.name}`,
    );
    checked += 1;
  }
  assert.ok(checked > 0, "expected at least one valid (200) fixture");
});

test("invalid-* fixtures exist for server-side validation testing", () => {
  const invalid = fixtures.filter((f) => f.expected_ingest_status !== 200);
  assert.ok(invalid.length > 0, "expected invalid-* fixtures");
  // Each invalid fixture declares the error codes the server should emit.
  for (const fx of invalid) {
    assert.ok(
      Array.isArray(fx.expected_error_codes) && fx.expected_error_codes.length > 0,
      `${fx.name} should declare expected_error_codes`,
    );
  }
});

// Large flush settings so these tests never auto-flush (no network) unless they
// explicitly mock fetch. The interval timer is unref'd, so it won't fire in the
// short test window and won't keep the process alive.
const NO_AUTOFLUSH = { flushIntervalSeconds: 3600, flushEventThreshold: 100_000 };

test("persists the pending queue and rehydrates from initialQueue", () => {
  const saved: HonchEvent[][] = [];
  const client = new HonchClient({
    apiKey: "k",
    deviceId: "dev-1",
    deviceModel: "m",
    firmwareVersion: "1.0",
    ...NO_AUTOFLUSH,
    onQueueChange: (q) => saved.push([...q]),
  });
  client.track("boot", undefined, 1_700_000_000_000);
  client.track("ping", undefined, 1_700_000_001_000);

  // onQueueChange fires on every enqueue; the latest snapshot is the full queue.
  const latest = saved.at(-1);
  assert.ok(latest);
  assert.deepEqual(
    latest.map((e) => e.event),
    ["boot", "ping"],
  );

  // A fresh client rehydrates persisted events, then appends new ones — exactly
  // the app-restart-after-offline path.
  const saved2: HonchEvent[][] = [];
  const restored = new HonchClient({
    apiKey: "k",
    deviceId: "dev-1",
    deviceModel: "m",
    firmwareVersion: "1.0",
    ...NO_AUTOFLUSH,
    initialQueue: latest,
    onQueueChange: (q) => saved2.push([...q]),
  });
  restored.track("resume", undefined, 1_700_000_002_000);
  assert.deepEqual(
    saved2.at(-1)?.map((e) => e.event),
    ["boot", "ping", "resume"],
  );
});

test("identify emits a $identify event with $anon_distinct_id (mocked fetch)", async () => {
  const calls: Array<{ url: string; body: Batch }> = [];
  const realFetch = globalThis.fetch;
  globalThis.fetch = (async (input: unknown, init?: { body?: unknown }) => {
    const bodyStr = typeof init?.body === "string" ? init.body : "{}";
    calls.push({ url: String(input), body: JSON.parse(bodyStr) as Batch });
    return new Response(JSON.stringify({ status: "ok", accepted: 1, rejected: 0, errors: [] }), {
      status: 200,
      headers: { "content-type": "application/json" },
    });
  }) as unknown as typeof fetch;

  try {
    const client = new HonchClient({
      apiKey: "k",
      deviceId: "dev-1",
      deviceModel: "m",
      firmwareVersion: "1.0",
      flushIntervalSeconds: 3600,
    });
    client.track("boot", undefined, 1_700_000_000_000); // anonymous (device id)
    await client.identify("user-1", { plan: "pro" }); // flushes boot, then emits $identify
    await client.close();
  } finally {
    globalThis.fetch = realFetch;
  }

  // The anonymous event went out under the device id.
  const bootCall = calls.find((c) => c.body.events.some((e) => e.event === "boot"));
  assert.ok(bootCall, "boot event was sent");
  assert.equal(bootCall.body.context.distinct_id, "dev-1");

  // The $identify event went out under the user id and names the device as anon,
  // which is what makes the server merge the two into one person.
  const idCall = calls.find((c) => c.body.events.some((e) => e.event === "$identify"));
  assert.ok(idCall, "a $identify event was sent");
  assert.equal(idCall.body.context.distinct_id, "user-1");
  const ev = idCall.body.events.find((e) => e.event === "$identify");
  const props = (ev?.properties ?? {}) as Record<string, unknown>;
  assert.equal(props.$anon_distinct_id, "dev-1");
  assert.deepEqual(props.$set, { plan: "pro" });
});

test("flush()/close() await the in-flight send and coalesce (no early resolve, no double-send)", async () => {
  const calls: Batch[] = [];
  let release: () => void = () => {};
  const realFetch = globalThis.fetch;
  globalThis.fetch = (async (_input: unknown, init?: { body?: unknown }) => {
    const bodyStr = typeof init?.body === "string" ? init.body : "{}";
    calls.push(JSON.parse(bodyStr) as Batch);
    // Block until released, simulating a slow in-flight request.
    await new Promise<void>((r) => {
      release = r;
    });
    return new Response(JSON.stringify({ status: "ok", accepted: 1, rejected: 0, errors: [] }), {
      status: 200,
      headers: { "content-type": "application/json" },
    });
  }) as unknown as typeof fetch;

  let closeResolved = false;
  try {
    const client = new HonchClient({
      apiKey: "k",
      deviceId: "dev-1",
      deviceModel: "m",
      firmwareVersion: "1.0",
      flushIntervalSeconds: 3600,
    });
    client.track("a", undefined, 1);

    const f1 = client.flush();
    const f2 = client.flush(); // must join the same in-flight run
    const closeP = client.close().then(() => {
      closeResolved = true;
    });

    // Let timers/microtasks run; the send is still blocked, so nothing drained.
    await new Promise((r) => setTimeout(r, 10));
    assert.equal(calls.length, 1, "exactly one POST started (coalesced, not duplicated)");
    assert.equal(closeResolved, false, "close() must NOT resolve while the send is in flight");

    release(); // unblock the in-flight request
    await Promise.all([f1, f2, closeP]);
    assert.equal(closeResolved, true, "close() resolves once the send completes");
    assert.equal(calls.length, 1, "still exactly one POST after draining");
  } finally {
    globalThis.fetch = realFetch;
  }
});
