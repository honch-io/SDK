import { mkdtemp } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { describe, expect, it } from "vitest";

import type { RelayDurableStore } from "../src/durableStore";
import { createDurableRelayQueue } from "../src/relayQueue";
import { createJsonFileRelayStore } from "../src/storage/jsonFileStore";
import { createMmkvRelayStore } from "../src/storage/mmkvStore";

function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

function frame(options: {
  sourceType?: number;
  first?: boolean;
  final?: boolean;
  sequence?: bigint;
  offset?: number;
  payload?: number[];
}): Uint8Array {
  const payload = new Uint8Array(options.payload ?? []);
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = options.sourceType ?? 1;
  bytes[2] = (options.first ? 1 : 0) | (options.final ? 2 : 0);
  let sequence = options.sequence ?? 1n;
  for (let i = 11; i >= 4; i -= 1) {
    bytes[i] = Number(sequence & 0xffn);
    sequence >>= 8n;
  }
  const offset = options.offset ?? 0;
  bytes[12] = (offset >>> 24) & 0xff;
  bytes[13] = (offset >>> 16) & 0xff;
  bytes[14] = (offset >>> 8) & 0xff;
  bytes[15] = offset & 0xff;
  bytes[16] = (payload.length >>> 8) & 0xff;
  bytes[17] = payload.length & 0xff;
  bytes.set(payload, 20);
  const crc = crc16(new Uint8Array([...bytes.slice(0, 18), ...payload]));
  bytes[18] = (crc >>> 8) & 0xff;
  bytes[19] = crc & 0xff;
  return bytes;
}

async function tempStorePath(): Promise<string> {
  const directory = await mkdtemp(join(tmpdir(), "honch-relay-store-"));
  return join(directory, "queue.json");
}

type StoreFactory = () => RelayDurableStore | Promise<RelayDurableStore>;
type StoreFactoryFactory = () => StoreFactory | Promise<StoreFactory>;

function runRelayDurableStoreConformance(name: string, createStoreFactory: StoreFactoryFactory): void {
  describe(`${name} restart behavior`, () => {
  it("continues an incomplete assembly after durable store re-instantiation", async () => {
    const createStore = await createStoreFactory();
    const firstQueue = createDurableRelayQueue(await createStore());

    await firstQueue.putChunk("device-a", frame({ first: true, sequence: 1n, payload: [1, 2] }));

    const restartedQueue = createDurableRelayQueue(await createStore());
    const result = await restartedQueue.putChunk(
      "device-a",
      frame({ final: true, sequence: 1n, offset: 2, payload: [3, 4] })
    );

    expect(result.complete).toBe(true);
    expect(Array.from((await restartedQueue.pending())[0]?.body ?? [])).toEqual([1, 2, 3, 4]);
  });

  it("keeps completed messages pending after durable store re-instantiation", async () => {
    const createStore = await createStoreFactory();
    const firstQueue = createDurableRelayQueue(await createStore());

    await firstQueue.putChunk(
      "device-a",
      frame({ first: true, final: true, sequence: 7n, payload: [9] })
    );

    const restartedQueue = createDurableRelayQueue(await createStore());

    expect((await restartedQueue.pending()).map((message) => message.sequence)).toEqual(["7"]);
  });
  });
}

runRelayDurableStoreConformance("JSON file relay durable store", async () => {
  const tempFile = await tempStorePath();
  return () => createJsonFileRelayStore(tempFile);
});

function fakeMmkv() {
  const values = new Map<string, string>();
  const setKeys: string[] = [];
  const removedKeys: string[] = [];
  return {
    values,
    setKeys,
    removedKeys,
    getString(key: string): string | undefined {
      return values.get(key);
    },
    set(key: string, value: string): void {
      setKeys.push(key);
      values.set(key, value);
    },
    remove(key: string): boolean {
      removedKeys.push(key);
      const existed = values.has(key);
      values.delete(key);
      return existed;
    }
  };
}

runRelayDurableStoreConformance("MMKV relay durable store", () => {
  const mmkv = fakeMmkv();
  return () => createMmkvRelayStore(mmkv);
});

describe("MMKV relay durable store retention", () => {
  it("stores chunk and message payloads under append-style keys instead of the legacy queue blob", async () => {
    const mmkv = fakeMmkv();
    const store = createMmkvRelayStore(mmkv, { now: () => 1000 });

    await store.putChunk({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      offset: 0,
      frameBytes: new Uint8Array([1, 2]),
      payload: new Uint8Array([2])
    });
    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([2])
    });

    expect(mmkv.setKeys).toContain("honch.relay.index.v2");
    expect(mmkv.setKeys).toContain("honch.relay.chunks.device-a.1.index.v1");
    expect(mmkv.setKeys).toContain("honch.relay.chunk.device-a.1.0");
    expect(mmkv.setKeys).toContain("honch.relay.message.device-a.1");
    expect(mmkv.setKeys).not.toContain("honch.relay.queue.v1");
  });

  it("stores binary MMKV records as base64 strings instead of JSON number arrays", async () => {
    const mmkv = fakeMmkv();
    const store = createMmkvRelayStore(mmkv, { now: () => 1000 });

    await store.putChunk({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      offset: 0,
      frameBytes: new Uint8Array([1, 2, 3, 4]),
      payload: new Uint8Array([2, 3, 4])
    });
    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([5, 6, 7])
    });

    const chunkRecord = mmkv.values.get("honch.relay.chunk.device-a.1.0") ?? "";
    const messageRecord = mmkv.values.get("honch.relay.message.device-a.1") ?? "";
    expect(chunkRecord).toContain('"frameBase64":"AQIDBA=="');
    expect(chunkRecord).toContain('"payloadBase64":"AgME"');
    expect(messageRecord).toContain('"bodyBase64":"BQYH"');
    expect(chunkRecord).not.toContain("[1,2,3,4]");
    expect(messageRecord).not.toContain("[5,6,7]");
    expect(Array.from((await store.chunks("device-a", "1"))[0]?.frameBytes ?? [])).toEqual([1, 2, 3, 4]);
    expect(Array.from((await store.completeMessages())[0]?.body ?? [])).toEqual([5, 6, 7]);
  });

  it("keeps relay keys under a caller-selected namespace for host-supplied MMKV stores", async () => {
    const mmkv = fakeMmkv();
    const store = createMmkvRelayStore(mmkv, {
      keyPrefix: "host.app.honch",
      now: () => 1000
    });

    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([1])
    });

    expect(mmkv.setKeys).toContain("host.app.honch.index.v2");
    expect(mmkv.setKeys).toContain("host.app.honch.message.device-a.1");
    expect(mmkv.setKeys).not.toContain("honch.relay.index.v2");
    expect(mmkv.setKeys).not.toContain("honch.relay.message.device-a.1");
  });

  it("migrates the legacy MMKV queue blob without dropping pending uploads", async () => {
    const mmkv = fakeMmkv();
    mmkv.set(
      "honch.relay.queue.v1",
      JSON.stringify({
        chunks: [],
        messages: [
          {
            deviceId: "device-a",
            sourceType: 1,
            sequence: "7",
            body: [7]
          }
        ]
      })
    );

    const store = createMmkvRelayStore(mmkv, { now: () => 1000 });

    expect((await store.completeMessages()).map((message) => message.sequence)).toEqual(["7"]);
    expect(mmkv.removedKeys).toContain("honch.relay.queue.v1");
    expect(mmkv.setKeys).toContain("honch.relay.message.device-a.7");
  });

  it("expires stale MMKV relay entries by TTL", async () => {
    let now = 1000;
    const mmkv = fakeMmkv();
    const store = createMmkvRelayStore(mmkv, { ttlMs: 100, now: () => now });

    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([1])
    });
    now = 1200;

    expect(await store.completeMessages()).toEqual([]);
  });

  it("bounds completed MMKV relay messages by dropping the oldest pending upload", async () => {
    let now = 1000;
    const mmkv = fakeMmkv();
    const store = createMmkvRelayStore(mmkv, { maxCompleteMessages: 1, now: () => now });

    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([1])
    });
    now = 1001;
    await store.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "2",
      body: new Uint8Array([2])
    });

    expect((await store.completeMessages()).map((message) => message.sequence)).toEqual(["2"]);
    expect(mmkv.removedKeys).toContain("honch.relay.message.device-a.1");
  });

  it("persists MMKV retry metadata across store re-instantiation", async () => {
    const mmkv = fakeMmkv();
    const firstStore = createMmkvRelayStore(mmkv, { now: () => 1000 });

    await firstStore.putCompleteMessage({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1",
      body: new Uint8Array([1])
    });
    await firstStore.markRetry("device-a", "1", { attempt: 3, nextAttemptAtMs: 9000 });

    const restartedStore = createMmkvRelayStore(mmkv, { now: () => 2000 });

    expect(await restartedStore.completeMessages()).toMatchObject([
      {
        deviceId: "device-a",
        sequence: "1",
        retryAttempt: 3,
        nextAttemptAtMs: 9000
      }
    ]);
  });
});
