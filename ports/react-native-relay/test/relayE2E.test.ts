import { mkdtemp } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { describe, expect, it, vi } from "vitest";

import {
  buildRelayFrame,
  loadWireV2CompactMessageFixture,
  runRelayCaptureE2E
} from "../e2e/relay-capture-e2e";

describe("relay capture E2E harness", () => {
  it("runs the relay receipt, offline retry, capture drain, and malformed rejection flow", async () => {
    const tempDir = await mkdtemp(join(tmpdir(), "honch-relay-e2e-"));
    const uploads: Uint8Array[] = [];

    const result = await runRelayCaptureE2E({
      tempFile: join(tempDir, "queue.json"),
      captureUrl: "https://capture.example.test",
      projectKey: "project-key",
      fetchImpl: vi.fn(async (_url, init) => {
        uploads.push(new Uint8Array(init?.body as ArrayBuffer));
        return new Response(null, { status: 204 });
      })
    });

    expect(result).toEqual({
      receipt: "PASS",
      retryPreservation: "PASS",
      uploadDrain: "PASS",
      malformedRejection: "PASS",
      clickHouseVerification: "SKIP"
    });
    expect(uploads).toHaveLength(1);
  });

  it("wraps compact wire-v2 fixture payloads in byte-accurate relay frames", async () => {
    const payload = await loadWireV2CompactMessageFixture("single-required-context");
    const frame = buildRelayFrame({ sequence: 11n, first: true, final: true, payload });

    expect(frame[0]).toBe(1);
    expect(frame[1]).toBe(1);
    expect(frame[2]).toBe(3);
    expect(frame[11]).toBe(11);
    expect(frame[17]).toBe(payload.length);
    expect(Array.from(frame.slice(20))).toEqual(Array.from(payload));
  });
});
