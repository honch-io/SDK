import { describe, expect, it } from "vitest";

import { buildRelayAck, createRelayFrameReceiver } from "../src/relayFrameReceiver";
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

describe("native relay interfaces", () => {
  it("builds relay ACK bytes for host-owned BLE writes", () => {
    expect(Array.from(buildRelayAck("1"))).toEqual([1, 0, 0, 0, 0, 0, 0, 0, 1]);
  });

  it("stores received host BLE frames and returns ACK bytes for completed messages", async () => {
    const queue = createInMemoryRelayQueue();
    const receiver = createRelayFrameReceiver({ queue });

    const result = await receiver.receiveFrame("device-a", frame({ first: true, final: true, payload: [9] }));

    expect(result.complete).toBe(true);
    expect(Array.from(result.message?.body ?? [])).toEqual([9]);
    expect(Array.from(result.ackBytes ?? [])).toEqual([1, 0, 0, 0, 0, 0, 0, 0, 1]);
  });

  it("does not ACK malformed frames", async () => {
    const receiver = createRelayFrameReceiver({ queue: createInMemoryRelayQueue() });
    const acknowledgements: string[] = [];

    await expect(
      receiver.receiveFrame("device-a", new Uint8Array([1, 2, 3]), {
        acknowledge: ({ deviceId }) => {
          acknowledgements.push(deviceId);
        }
      })
    ).rejects.toThrow();

    expect(acknowledgements).toEqual([]);
  });

  it("ACKs only after a multi-chunk message is durably complete", async () => {
    const acknowledgements: number[][] = [];
    const receiver = createRelayFrameReceiver({ queue: createInMemoryRelayQueue() });

    await receiver.receiveFrame("device-a", frame({ first: true, payload: [1, 2] }), {
      acknowledge: ({ ackBytes }) => {
        acknowledgements.push(Array.from(ackBytes));
      }
    });
    expect(acknowledgements).toEqual([]);

    await receiver.receiveFrame("device-a", frame({ final: true, offset: 2, payload: [3] }), {
      acknowledge: ({ ackBytes }) => {
        acknowledgements.push(Array.from(ackBytes));
      }
    });
    expect(acknowledgements).toEqual([[1, 0, 0, 0, 0, 0, 0, 0, 1]]);
  });

  it("re-ACKs duplicate completed frames so host retries can settle", async () => {
    const acknowledgements: string[] = [];
    const receiver = createRelayFrameReceiver({ queue: createInMemoryRelayQueue() });
    const completedFrame = frame({ first: true, final: true, payload: [9] });

    for (let index = 0; index < 2; index += 1) {
      await receiver.receiveFrame("device-a", completedFrame, {
        acknowledge: ({ deviceId, sequence }) => {
          acknowledgements.push(`${deviceId}:${sequence}`);
        }
      });
    }

    expect(acknowledgements).toEqual(["device-a:1", "device-a:1"]);
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
