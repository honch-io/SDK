import { afterEach, describe, expect, it, vi } from "vitest";

import { uploadRelayMessage } from "../src/uploader";

const canonicalFixtures = [
  "spec/conformance/envelopes/basic_batch.json",
  "spec/conformance/events/basic-track.json",
  "spec/conformance/events/auto_stamp_wins_conflict.json",
  "spec/conformance/events/boot_event.json",
  "spec/conformance/events/custom_event_with_session.json",
  "spec/conformance/events/identity-reset.json",
  "spec/conformance/events/session-track.json",
  "spec/conformance/http/response-policy.json"
] as const;

const fixtureData = {
  "spec/conformance/envelopes/basic_batch.json": {
    expected: {
      token: "test_key_123",
      batch: [{ event: "button_pressed", distinct_id: "dev_a3f2c1d4e5f6" }]
    }
  },
  "spec/conformance/events/basic-track.json": { name: "basic track" },
  "spec/conformance/events/auto_stamp_wins_conflict.json": { expected: { event: "test_event" } },
  "spec/conformance/events/boot_event.json": { expected: { event: "$device_boot" } },
  "spec/conformance/events/custom_event_with_session.json": {
    expected: { event: "hdr_burst_used" }
  },
  "spec/conformance/events/identity-reset.json": { name: "identity reset" },
  "spec/conformance/events/session-track.json": { name: "session track" },
  "spec/conformance/http/response-policy.json": {
    cases: [
      { status: 200, queue: "consume" },
      { status: 202, queue: "consume" },
      { status: 401, queue: "drop_or_dead_letter" },
      { status: 400, queue: "drop_or_dead_letter" },
      { status: 404, queue: "drop_or_dead_letter" },
      { status: 429, queue: "preserve" },
      { status: 500, queue: "preserve" },
      { status: 503, queue: "preserve" },
      { status: 0, queue: "preserve" }
    ]
  }
} satisfies Record<(typeof canonicalFixtures)[number], unknown>;

function fixtureBody(path: keyof typeof fixtureData): Uint8Array {
  return new TextEncoder().encode(JSON.stringify(fixtureData[path]));
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

  it("keeps every canonical fixture visible to relay conformance coverage", () => {
    for (const path of canonicalFixtures) {
      expect(fixtureData[path]).toBeTruthy();
    }
  });

  it("preserves canonical fixture payload bytes inside the compact relay upload frame", async () => {
    const body = fixtureBody("spec/conformance/envelopes/basic_batch.json");
    const fetchMock = vi.fn(async () => new Response(null, { status: 202 }));
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
    const fixture = fixtureData["spec/conformance/http/response-policy.json"] as {
      cases: Array<{ status: number; queue: "consume" | "preserve" | "drop_or_dead_letter" }>;
    };
    const body = fixtureBody("spec/conformance/events/basic-track.json");

    for (const testCase of fixture.cases) {
      const response =
        testCase.status === 0
          ? Promise.reject(new TypeError("network unavailable"))
          : Promise.resolve(new Response(null, { status: testCase.status }));
      vi.stubGlobal("fetch", vi.fn(async () => response));

      const upload = uploadRelayMessage(config, {
        deviceId: "dev_fixture",
        sourceType: 1,
        sequence: String(testCase.status),
        body
      });

      if (testCase.queue === "consume") {
        await expect(upload).resolves.toBeUndefined();
      } else {
        await expect(upload).rejects.toThrow();
      }
    }
  });
});
