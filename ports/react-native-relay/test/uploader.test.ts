import { afterEach, describe, expect, it, vi } from "vitest";

import {
  buildRelayUploadBuffer,
  uploadRelayMessage,
  uploadRelayMessageOutcome
} from "../src/uploader";
import { createInMemoryRelayQueue } from "../src/relayQueue";
import { nextBackoffDelayMs } from "../src/retry";

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
  first?: boolean;
  final?: boolean;
  sequence?: bigint;
  payload?: number[];
}): Uint8Array {
  const payload = new Uint8Array(options.payload ?? []);
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = 1;
  bytes[2] = (options.first ? 1 : 0) | (options.final ? 2 : 0);
  let sequence = options.sequence ?? 1n;
  for (let i = 11; i >= 4; i -= 1) {
    bytes[i] = Number(sequence & 0xffn);
    sequence >>= 8n;
  }
  bytes[17] = payload.length & 0xff;
  bytes.set(payload, 20);
  const crc = crc16(new Uint8Array([...bytes.slice(0, 18), ...payload]));
  bytes[18] = (crc >>> 8) & 0xff;
  bytes[19] = crc & 0xff;
  return bytes;
}

const config = {
  endpointUrl: "https://capture.example.test/",
  projectKey: "project-key",
  relayId: "relay-1",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0",
  streamId: () => "relay-stream",
  messageId: () => 7
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

  it("uploads relay messages to the capture endpoint with relay headers", async () => {
    const fetchMock = vi.fn(async () => new Response(null, { status: 202 }));
    vi.stubGlobal("fetch", fetchMock);

    await uploadRelayMessage(config, message);

    expect(fetchMock).toHaveBeenCalledWith("https://capture.example.test/capture", {
      method: "POST",
      headers: expect.objectContaining({
        "Content-Type": "application/vnd.honch.chunk",
        "X-Honch-Project-Key": "project-key",
        "X-Honch-Stream-Id": "relay-stream",
        "X-Honch-Relay-Id": "relay-1",
        "X-Honch-Relay-SDK-Platform": "react-native",
        "X-Honch-Relay-SDK-Version": "0.1.0"
      }),
      body: expect.any(ArrayBuffer)
    });
  });

  it("does not use the legacy batch CBOR upload contract", async () => {
    const fetchMock = vi.fn(async () => new Response(null, { status: 202 }));
    vi.stubGlobal("fetch", fetchMock);

    await uploadRelayMessage(config, message);

    const [fetchUrl, request] = fetchMock.mock.calls[0] as unknown as [
      string,
      { headers: Record<string, string> }
    ];
    expect(fetchUrl).not.toContain("/batch");
    expect(request.headers["Content-Type"]).not.toBe("application/cbor");
    expect(request.headers).not.toHaveProperty("Authorization");
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

  it("builds a capture upload buffer without consuming queue state", async () => {
    const queue = createInMemoryRelayQueue();
    await queue.putChunk(
      "device-a",
      frame({ first: true, final: true, sequence: 7n, payload: [1, 2, 3] })
    );
    const [pending] = await queue.pending();

    const body = buildRelayUploadBuffer(config, pending);

    expect(Array.from(new Uint8Array(body))).toEqual([2, 7, 1, 2, 3, 0xad, 0xad]);
    expect((await queue.pending()).map((queued) => queued.sequence)).toEqual(["7"]);
  });

  it("computes capped exponential backoff with deterministic jitter", () => {
    expect(nextBackoffDelayMs(0, () => 0.5)).toBe(1000);
    expect(nextBackoffDelayMs(1, () => 0.5)).toBe(2000);
    expect(nextBackoffDelayMs(2, () => 1)).toBe(5000);
    expect(nextBackoffDelayMs(20, () => 0.5)).toBe(300000);
  });
});
