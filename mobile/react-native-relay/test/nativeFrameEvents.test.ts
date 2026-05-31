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
});
