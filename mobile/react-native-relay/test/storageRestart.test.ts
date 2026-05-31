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
  return {
    getString(key: string): string | undefined {
      return values.get(key);
    },
    set(key: string, value: string): void {
      values.set(key, value);
    },
    remove(key: string): boolean {
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
