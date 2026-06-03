import { describe, expect, it } from "vitest";

import { decodeRelayFrameEventPayload, subscribeRelayNativeFrames } from "../src/nativeFrameEvents";

describe("native frame events", () => {
  it("decodes base64 frame payloads without relying on Buffer at runtime", () => {
    expect(Array.from(decodeRelayFrameEventPayload("AQIDBA=="))).toEqual([1, 2, 3, 4]);
  });

  it("subscribes to HonchRelayFrame and forwards decoded bytes", () => {
    const received: Array<{ deviceId: string; bytes: number[] }> = [];
    const subscription = subscribeRelayNativeFrames(
      {
        addListener(eventName, listener) {
          expect(eventName).toBe("HonchRelayFrame");
          listener({ deviceId: "device-a", frameBase64: "CQgH" });
          return { remove() {} };
        }
      },
      async (deviceId, frameBytes) => {
        received.push({ deviceId, bytes: Array.from(frameBytes) });
      }
    );

    subscription.remove();

    expect(received).toEqual([{ deviceId: "device-a", bytes: [9, 8, 7] }]);
  });

  it("drops malformed native frame payloads without calling the frame handler", () => {
    const received: string[] = [];
    subscribeRelayNativeFrames(
      {
        addListener(_eventName, listener) {
          listener({ deviceId: "device-a", frameBase64: "!!!!" });
          return { remove() {} };
        }
      },
      (deviceId) => {
        received.push(deviceId);
      }
    );

    expect(received).toEqual([]);
  });

  it("catches async frame handler rejections from native event dispatch", async () => {
    const unhandled: unknown[] = [];
    const previousUnhandled = process.listeners("unhandledRejection");
    process.removeAllListeners("unhandledRejection");
    process.on("unhandledRejection", (error) => {
      unhandled.push(error);
    });

    try {
      subscribeRelayNativeFrames(
        {
          addListener(_eventName, listener) {
            listener({ deviceId: "device-a", frameBase64: "AQID" });
            return { remove() {} };
          }
        },
        async () => {
          throw new Error("durable receipt failed");
        }
      );

      await new Promise((resolve) => setTimeout(resolve, 0));
      expect(unhandled).toEqual([]);
    } finally {
      process.removeAllListeners("unhandledRejection");
      for (const listener of previousUnhandled) {
        process.on("unhandledRejection", listener);
      }
    }
  });
});
