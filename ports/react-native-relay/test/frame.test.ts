import { describe, expect, it } from "vitest";

import { decodeRelayFrame } from "../src/frame";

describe("decodeRelayFrame", () => {
  it("decodes a valid empty payload final frame", () => {
    const frame = new Uint8Array(20);
    frame[0] = 1;
    frame[1] = 1;
    frame[2] = 3;
    frame[11] = 7;

    const decoded = decodeRelayFrame(frame);

    expect(decoded.version).toBe(1);
    expect(decoded.sourceType).toBe(1);
    expect(decoded.first).toBe(true);
    expect(decoded.final).toBe(true);
    expect(decoded.sequence).toBe(7n);
    expect(decoded.offset).toBe(0);
    expect(decoded.payload.length).toBe(0);
  });

  it("rejects unsupported versions", () => {
    const frame = new Uint8Array(20);
    frame[0] = 2;

    expect(() => decodeRelayFrame(frame)).toThrow("unsupported relay frame version");
  });
});
