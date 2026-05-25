import { mkdtemp } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { afterEach, describe, expect, it, vi } from "vitest";

import {
  buildRelayFrame,
  loadWireV2CompactMessageFixture,
  runRelayCaptureE2E
} from "../e2e/relay-capture-e2e";

describe("relay capture E2E harness", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("fails early when live capture health is unavailable", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 503 })));
    const tempDir = await mkdtemp(join(tmpdir(), "honch-relay-e2e-"));

    await expect(
      runRelayCaptureE2E({
        tempFile: join(tempDir, "queue.json"),
        captureUrl: "https://capture.example.test",
        projectKey: "project-key"
      })
    ).rejects.toThrow("capture preflight failed: GET https://capture.example.test/health returned 503");
  });

  it("fails early when ClickHouse health is unavailable", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(null, { status: 500 })));
    const tempDir = await mkdtemp(join(tmpdir(), "honch-relay-e2e-"));

    await expect(
      runRelayCaptureE2E({
        tempFile: join(tempDir, "queue.json"),
        captureUrl: "https://capture.example.test",
        projectKey: "project-key",
        clickHouseUrl: "https://clickhouse.example.test",
        fetchImpl: vi.fn(async () => new Response(null, { status: 204 }))
      })
    ).rejects.toThrow("ClickHouse preflight failed: GET https://clickhouse.example.test/ping returned 500");
  });

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
