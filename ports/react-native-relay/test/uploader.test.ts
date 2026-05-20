import { afterEach, describe, expect, it, vi } from "vitest";

import { uploadRelayMessage } from "../src/uploader";

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
});
