import { describe, expect, it } from "vitest";

import { createBleRelayReceiver } from "../src/ble";
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

function frame(payload: number[]): Uint8Array {
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = 1;
  bytes[2] = 3;
  bytes[11] = 1;
  bytes[16] = (payload.length >>> 8) & 0xff;
  bytes[17] = payload.length & 0xff;
  bytes.set(payload, 20);
  const crc = crc16(new Uint8Array([...bytes.slice(0, 18), ...payload]));
  bytes[18] = (crc >>> 8) & 0xff;
  bytes[19] = crc & 0xff;
  return bytes;
}

describe("native relay interfaces", () => {
  it("stores received BLE frames and ACKs completed messages", async () => {
    const queue = createInMemoryRelayQueue();
    const acknowledgements: string[] = [];
    const receiver = createBleRelayReceiver({
      queue,
      native: {
        async startScan() {},
        async stopScan() {},
        async connect() {},
        async disconnect() {},
        async acknowledgeMessage(deviceId, sequence) {
          acknowledgements.push(`${deviceId}:${sequence}`);
        }
      }
    });

    const result = await receiver.receiveFrame("device-a", frame([9]));

    expect(result.complete).toBe(true);
    expect(Array.from(result.message?.body ?? [])).toEqual([9]);
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
