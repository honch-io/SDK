import { describe, expect, it } from "vitest";

import { createInMemoryRelayQueue, createDurableRelayQueue } from "../src/relayQueue";
import { createMemoryDurableStore } from "../src/durableStore";

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

describe("createInMemoryRelayQueue", () => {
  it("only completes and returns a message after the final chunk", async () => {
    const queue = createInMemoryRelayQueue();

    const first = await queue.putChunk("device-a", frame({ first: true, payload: [1, 2] }));
    expect(first.complete).toBe(false);
    expect(await queue.pending()).toEqual([]);

    const final = await queue.putChunk(
      "device-a",
      frame({ final: true, offset: 2, payload: [3, 4] })
    );

    expect(final.complete).toBe(true);
    expect(final.message).toMatchObject({
      deviceId: "device-a",
      sourceType: 1,
      sequence: "1"
    });
    expect(Array.from(final.message?.body ?? [])).toEqual([1, 2, 3, 4]);
  });

  it("treats an exact duplicate chunk as idempotent", async () => {
    const queue = createInMemoryRelayQueue();
    const chunk = frame({ first: true, final: true, payload: [9] });

    const first = await queue.putChunk("device-a", chunk);
    const duplicate = await queue.putChunk("device-a", chunk);

    expect(first.complete).toBe(true);
    expect(duplicate.complete).toBe(true);
    expect(Array.from(duplicate.message?.body ?? [])).toEqual([9]);
    expect(await queue.pending()).toHaveLength(1);
  });

  it("rejects a duplicate chunk with different bytes", async () => {
    const queue = createInMemoryRelayQueue();

    await queue.putChunk("device-a", frame({ first: true, payload: [1, 2] }));

    await expect(
      queue.putChunk("device-a", frame({ first: true, payload: [8, 8] }))
    ).rejects.toThrow("relay duplicate chunk mismatch");
  });

  it("rejects a second final chunk that changes the completed message length", async () => {
    const queue = createInMemoryRelayQueue();

    await queue.putChunk("device-a", frame({ first: true, final: true, payload: [1, 2] }));

    await expect(
      queue.putChunk("device-a", frame({ final: true, offset: 2, payload: [3] }))
    ).rejects.toThrow("relay final chunk length mismatch");
  });

  it("returns complete unuploaded messages and removes uploaded messages", async () => {
    const queue = createInMemoryRelayQueue();

    await queue.putChunk("device-a", frame({ first: true, final: true, payload: [1] }));
    await queue.putChunk(
      "device-b",
      frame({ first: true, final: true, sequence: 2n, payload: [2] })
    );

    expect((await queue.pending()).map((message) => message.deviceId)).toEqual([
      "device-a",
      "device-b"
    ]);

    await queue.markUploaded("device-a", "1");

    expect((await queue.pending()).map((message) => message.deviceId)).toEqual(["device-b"]);
  });
});

describe("createDurableRelayQueue", () => {
  it("continues an incomplete assembly after queue re-instantiation", async () => {
    const store = createMemoryDurableStore();
    const firstQueue = createDurableRelayQueue(store);

    await firstQueue.putChunk("device-a", frame({ first: true, payload: [1, 2] }));

    const restartedQueue = createDurableRelayQueue(store);
    const final = await restartedQueue.putChunk(
      "device-a",
      frame({ final: true, offset: 2, payload: [3, 4] })
    );

    expect(final.complete).toBe(true);
    expect(Array.from(final.message?.body ?? [])).toEqual([1, 2, 3, 4]);
    expect((await restartedQueue.pending()).map((message) => message.sequence)).toEqual(["1"]);
  });

  it("keeps complete messages pending across queue re-instantiation until upload is marked", async () => {
    const store = createMemoryDurableStore();
    const firstQueue = createDurableRelayQueue(store);

    await firstQueue.putChunk("device-a", frame({ first: true, final: true, payload: [9] }));

    const restartedQueue = createDurableRelayQueue(store);
    expect((await restartedQueue.pending()).map((message) => Array.from(message.body))).toEqual([[9]]);

    await restartedQueue.markUploaded("device-a", "1");

    const uploadedQueue = createDurableRelayQueue(store);
    expect(await uploadedQueue.pending()).toEqual([]);
  });

  it("persists retry metadata across queue re-instantiation", async () => {
    const store = createMemoryDurableStore();
    const firstQueue = createDurableRelayQueue(store);

    await firstQueue.putChunk("device-a", frame({ first: true, final: true, payload: [9] }));
    await firstQueue.markRetry("device-a", "1", { attempt: 2, nextAttemptAtMs: 12345 });

    const restartedQueue = createDurableRelayQueue(store);

    expect(await restartedQueue.pending()).toMatchObject([
      {
        deviceId: "device-a",
        sequence: "1",
        retryAttempt: 2,
        nextAttemptAtMs: 12345
      }
    ]);
  });
});
