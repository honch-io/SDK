import { afterEach, describe, expect, it, vi } from "vitest";

import { createMobileRelay } from "../src/mobileRelay";
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

function base64(bytes: Uint8Array): string {
  return Buffer.from(bytes).toString("base64");
}

const uploaderConfig = {
  endpointUrl: "https://capture.example",
  projectKey: "test-key",
  relayId: "relay-1",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0",
  streamId: () => "relay-stream",
  messageId: (message: { sequence: string }) => Number(message.sequence)
};

describe("createMobileRelay", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("ACKs durable BLE receipt and keeps retryable uploads pending", async () => {
    const acknowledgements: number[][] = [];
    const scheduled: number[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      random: () => 0.5,
      schedulerNative: {
        async scheduleUpload(delayMs) {
          scheduled.push(delayMs);
        },
        async cancelUpload() {}
      }
    });

    await relay.receiveFrame("device-a", frame([1, 2, 3]), {
      acknowledge: ({ ackBytes }) => {
        acknowledgements.push(Array.from(ackBytes));
      }
    });
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 503 })));

    await relay.drainUploads();

    expect(acknowledgements).toEqual([[1, 0, 0, 0, 0, 0, 0, 0, 1]]);
    expect(scheduled).toEqual([1000]);
    expect((await relay.pending()).map((message) => message.sequence)).toEqual(["1"]);
  });

  it("removes uploaded payloads after capture accepts them", async () => {
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      schedulerNative: {
        async scheduleUpload() {},
        async cancelUpload() {}
      }
    });
    await relay.receiveFrame("device-a", frame([4]));
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 202 })));

    await relay.drainUploads();

    expect(await relay.pending()).toEqual([]);
  });

  it("drains pending uploads when the upload scheduler starts", async () => {
    const scheduled: number[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      schedulerNative: {
        async scheduleUpload(delayMs) {
          scheduled.push(delayMs);
        },
        async cancelUpload() {}
      }
    });
    await relay.receiveFrame("device-a", frame([5]));
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 202 })));

    await relay.startUploadScheduler();

    expect(await relay.pending()).toEqual([]);
    expect(scheduled).toEqual([0]);
  });

  it("stops the native upload scheduler", async () => {
    const calls: string[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      schedulerNative: {
        async scheduleUpload(delayMs) {
          calls.push(`schedule:${delayMs}`);
        },
        async cancelUpload() {
          calls.push("cancel");
        }
      }
    });

    await relay.stopUploadScheduler();

    expect(calls).toEqual(["cancel"]);
  });

  it("drains in foreground when no native upload scheduler is configured", async () => {
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig
    });
    await relay.receiveFrame("device-a", frame([6]));
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 202 })));

    await relay.startUploadScheduler();

    expect(await relay.pending()).toEqual([]);
  });

  it("receives host-owned frame events into durable receipt", async () => {
    const listeners = new Map<string, (event: { deviceId: string; frameBase64: string }) => void>();
    const acknowledgements: string[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      schedulerNative: {
        async scheduleUpload() {},
        async cancelUpload() {}
      }
    });

    const subscription = {
      remove() {
        listeners.delete("host-frame");
      }
    };
    listeners.set("host-frame", (event) => {
      void relay.receiveFrame(
        event.deviceId,
        Uint8Array.from(Buffer.from(event.frameBase64, "base64")),
        {
          acknowledge: ({ deviceId, sequence }) => {
            acknowledgements.push(`${deviceId}:${sequence}`);
          }
        }
      );
    });
    listeners.get("host-frame")?.({
      deviceId: "device-a",
      frameBase64: base64(frame([6, 7]))
    });

    await new Promise((resolve) => setTimeout(resolve, 0));

    expect(acknowledgements).toEqual(["device-a:1"]);
    expect((await relay.pending()).map((message) => Array.from(message.body))).toEqual([[6, 7]]);

    subscription.remove();
    expect(listeners.has("host-frame")).toBe(false);
  });
});
