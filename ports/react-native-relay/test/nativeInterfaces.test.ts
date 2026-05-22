import { describe, expect, it } from "vitest";

import { createBleRelayReceiver } from "../src/ble";
import type { RelayBleNative } from "../src/ble";
import { createRelayUploadScheduler } from "../src/scheduler";
import { createInMemoryRelayQueue } from "../src/relayQueue";

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
  payload: number[];
  first?: boolean;
  final?: boolean;
  offset?: number;
}): Uint8Array {
  const payload = new Uint8Array(options.payload);
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = 1;
  bytes[2] = (options.first ? 1 : 0) | (options.final ? 2 : 0);
  bytes[11] = 1;
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

function fakeNative(acknowledgements: string[]): RelayBleNative {
  return {
    async startScan() {},
    async stopScan() {},
    async connect() {},
    async disconnect() {},
    async subscribeFrames() {},
    async acknowledgeMessage(deviceId, sequence) {
      acknowledgements.push(`${deviceId}:${sequence}`);
    }
  };
}

describe("native relay interfaces", () => {
  it("stores received BLE frames and ACKs completed messages", async () => {
    const queue = createInMemoryRelayQueue();
    const acknowledgements: string[] = [];
    const receiver = createBleRelayReceiver({
      queue,
      native: fakeNative(acknowledgements)
    });

    const result = await receiver.receiveFrame("device-a", frame({ first: true, final: true, payload: [9] }));

    expect(result.complete).toBe(true);
    expect(Array.from(result.message?.body ?? [])).toEqual([9]);
    expect(acknowledgements).toEqual(["device-a:1"]);
  });

  it("subscribes frame notifications through the native BLE bridge", async () => {
    const subscriptions: string[] = [];
    const receiver = createBleRelayReceiver({
      queue: createInMemoryRelayQueue(),
      native: {
        ...fakeNative([]),
        async subscribeFrames(deviceId) {
          subscriptions.push(deviceId);
        }
      }
    });

    await receiver.subscribeFrames("device-a");

    expect(subscriptions).toEqual(["device-a"]);
  });

  it("does not ACK malformed frames", async () => {
    const acknowledgements: string[] = [];
    const receiver = createBleRelayReceiver({
      queue: createInMemoryRelayQueue(),
      native: fakeNative(acknowledgements)
    });

    await expect(receiver.receiveFrame("device-a", new Uint8Array([1, 2, 3]))).rejects.toThrow();

    expect(acknowledgements).toEqual([]);
  });

  it("ACKs only after a multi-chunk message is durably complete", async () => {
    const acknowledgements: string[] = [];
    const receiver = createBleRelayReceiver({
      queue: createInMemoryRelayQueue(),
      native: fakeNative(acknowledgements)
    });

    await receiver.receiveFrame("device-a", frame({ first: true, payload: [1, 2] }));
    expect(acknowledgements).toEqual([]);

    await receiver.receiveFrame("device-a", frame({ final: true, offset: 2, payload: [3] }));
    expect(acknowledgements).toEqual(["device-a:1"]);
  });

  it("delegates upload scheduling to the native scheduler", async () => {
    const calls: number[] = [];
    const scheduler = createRelayUploadScheduler({
      native: {
        async scheduleUpload(delayMs) {
          calls.push(delayMs);
        },
        async cancelUpload() {
          calls.push(-1);
        }
      }
    });

    await scheduler.schedule(2500);
    await scheduler.cancel();

    expect(calls).toEqual([2500, -1]);
  });
});
