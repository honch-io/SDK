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
    const acknowledgements: string[] = [];
    const scheduled: number[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      random: () => 0.5,
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage(deviceId, sequence) {
          acknowledgements.push(`${deviceId}:${sequence}`);
        }
      },
      schedulerNative: {
        async scheduleUpload(delayMs) {
          scheduled.push(delayMs);
        },
        async cancelUpload() {}
      }
    });

    await relay.receiveFrame("device-a", frame([1, 2, 3]));
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 503 })));

    await relay.drainUploads();

    expect(acknowledgements).toEqual(["device-a:1"]);
    expect(scheduled).toEqual([1000]);
    expect((await relay.pending()).map((message) => message.sequence)).toEqual(["1"]);
  });

  it("removes uploaded payloads after capture accepts them", async () => {
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage() {}
      },
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
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage() {}
      },
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
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage() {}
      },
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

  it("exposes discovered BLE relay devices", async () => {
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [{ id: "device-a", name: "Relay A", rssi: -64 }];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage() {}
      },
      schedulerNative: {
        async scheduleUpload() {},
        async cancelUpload() {}
      }
    });

    await expect(relay.discoveredDevices()).resolves.toEqual([
      { id: "device-a", name: "Relay A", rssi: -64 }
    ]);
  });

  it("subscribes native frame events into durable BLE receipt", async () => {
    const listeners = new Map<string, (event: { deviceId: string; frameBase64: string }) => void>();
    const acknowledgements: string[] = [];
    const relay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig,
      frameEvents: {
        addListener(eventName, listener) {
          listeners.set(eventName, listener);
          return {
            remove() {
              listeners.delete(eventName);
            }
          };
        }
      },
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage(deviceId, sequence) {
          acknowledgements.push(`${deviceId}:${sequence}`);
        }
      },
      schedulerNative: {
        async scheduleUpload() {},
        async cancelUpload() {}
      }
    });

    const subscription = relay.subscribeNativeFrames();
    listeners.get("HonchRelayFrame")?.({
      deviceId: "device-a",
      frameBase64: base64(frame([6, 7]))
    });

    await new Promise((resolve) => setTimeout(resolve, 0));

    expect(acknowledgements).toEqual(["device-a:1"]);
    expect((await relay.pending()).map((message) => Array.from(message.body))).toEqual([[6, 7]]);

    subscription.remove();
    expect(listeners.has("HonchRelayFrame")).toBe(false);
  });
});
