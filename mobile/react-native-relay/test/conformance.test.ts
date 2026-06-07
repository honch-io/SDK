import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";

import { uploadRelayMessage, uploadRelayMessageOutcome } from "../src/uploader";

type ResponsePolicyFixture = {
  cases: Array<{ status: number; queue: "consume" | "preserve" | "drop_or_dead_letter" }>;
};

function loadFixture<T>(path: string): T {
  return JSON.parse(readFileSync(join(import.meta.dirname, "..", "..", "..", path), "utf8")) as T;
}

function fixtureBody(path: string): Uint8Array {
  return new TextEncoder().encode(readFileSync(join(import.meta.dirname, "..", "..", "..", path), "utf8"));
}

const config = {
  endpointUrl: "https://capture.example/",
  projectKey: "test-key",
  relayId: "relay-1",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0",
  streamId: () => "relay-stream",
  messageId: (message: { sequence: string }) => Number(message.sequence)
};

describe("React Native relay conformance fixtures", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("preserves canonical fixture payload bytes inside the compact relay upload frame", async () => {
    const body = fixtureBody("spec/conformance/events/basic-track.json");
    const fetchMock = vi.fn(async () => new Response(null, { status: 204 }));
    vi.stubGlobal("fetch", fetchMock);

    await uploadRelayMessage(config, {
      deviceId: "dev_a3f2c1d4e5f6",
      sourceType: 1,
      sequence: "1",
      body
    });

    const calls = fetchMock.mock.calls as unknown as Array<[string, { body: ArrayBuffer }]>;
    const request = calls[0]?.[1];
    expect(request).toBeDefined();
    if (request === undefined) {
      throw new Error("relay upload request was not captured");
    }
    const frame = new Uint8Array(request.body as ArrayBuffer);
    expect(Array.from(frame.slice(2, -2))).toEqual(Array.from(body));
  });

  it("implements the canonical response policy for relay upload statuses", async () => {
    const fixture = loadFixture<ResponsePolicyFixture>("spec/conformance/http/response-policy.json");
    const body = fixtureBody("spec/conformance/events/basic-track.json");

    for (const testCase of fixture.cases) {
      const response =
        testCase.status === 0
          ? Promise.reject(new TypeError("network unavailable"))
          : Promise.resolve(new Response(null, { status: testCase.status }));
      vi.stubGlobal("fetch", vi.fn(async () => response));

      const upload = uploadRelayMessageOutcome(config, {
        deviceId: "dev_fixture",
        sourceType: 1,
        sequence: String(testCase.status),
        body
      });

      await expect(upload).resolves.toMatchObject({
        action:
          testCase.queue === "consume"
            ? "consume"
            : testCase.queue === "drop_or_dead_letter"
              ? "drop"
              : "retry"
      });
    }
  });
});
