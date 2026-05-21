import { afterEach, describe, expect, it, vi } from "vitest";

import { uploadRelayMessage, uploadRelayMessageOutcome } from "../src/uploader";
import { nextBackoffDelayMs } from "../src/retry";

const config = {
  endpointUrl: "https://capture.example/",
  apiKey: "test-key",
  relayId: "relay-1",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0"
};

const message = {
  deviceId: "device-a",
  sourceType: 1,
  sequence: "7",
  body: new Uint8Array([1, 2, 3])
};

describe("uploadRelayMessage", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("uploads relay messages to the batch endpoint with relay headers", async () => {
    const fetchMock = vi.fn(async () => new Response(null, { status: 202 }));
    vi.stubGlobal("fetch", fetchMock);

    await uploadRelayMessage(config, message);

    expect(fetchMock).toHaveBeenCalledWith("https://capture.example/batch", {
      method: "POST",
      headers: {
        "Content-Type": "application/cbor",
        "X-Honch-Relay-Id": "relay-1",
        "X-Honch-Relay-SDK-Platform": "react-native",
        "X-Honch-Relay-SDK-Version": "0.1.0",
        Authorization: "Bearer test-key"
      },
      body: expect.any(ArrayBuffer)
    });
  });

  it("rejects failed relay uploads with the status code", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 500 })));

    await expect(uploadRelayMessage(config, message)).rejects.toThrow("relay upload failed: 500");
  });

  it("classifies upload statuses using the canonical relay queue policy", async () => {
    const cases = [
      { status: 200, action: "consume" },
      { status: 202, action: "consume" },
      { status: 400, action: "drop" },
      { status: 401, action: "drop" },
      { status: 404, action: "drop" },
      { status: 429, action: "retry" },
      { status: 500, action: "retry" },
      { status: 503, action: "retry" }
    ] as const;

    for (const testCase of cases) {
      vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: testCase.status })));

      await expect(uploadRelayMessageOutcome(config, message)).resolves.toMatchObject({
        action: testCase.action,
        status: testCase.status
      });
    }
  });

  it("classifies network failures as retryable", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => Promise.reject(new TypeError("offline"))));

    await expect(uploadRelayMessageOutcome(config, message)).resolves.toMatchObject({
      action: "retry"
    });
  });

  it("computes capped exponential backoff with deterministic jitter", () => {
    expect(nextBackoffDelayMs(0, () => 0.5)).toBe(1000);
    expect(nextBackoffDelayMs(1, () => 0.5)).toBe(2000);
    expect(nextBackoffDelayMs(2, () => 1)).toBe(5000);
    expect(nextBackoffDelayMs(20, () => 0.5)).toBe(300000);
  });
});
