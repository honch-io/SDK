import { describe, expect, it } from "vitest";

import { buildSingleWireV2Frame, crc16CcittFalse, encodeUvarint } from "../src/wireV2";

describe("wire-v2 frame helpers", () => {
  it("encodes unsigned varints using LEB128", () => {
    expect(Array.from(encodeUvarint(0))).toEqual([0]);
    expect(Array.from(encodeUvarint(127))).toEqual([127]);
    expect(Array.from(encodeUvarint(128))).toEqual([128, 1]);
    expect(Array.from(encodeUvarint(16_384))).toEqual([128, 128, 1]);
  });

  it("rejects invalid message ids", () => {
    expect(() => encodeUvarint(-1)).toThrow("wire-v2 message id");
    expect(() => encodeUvarint(1.5)).toThrow("wire-v2 message id");
    expect(() => encodeUvarint(Number.MAX_SAFE_INTEGER + 1)).toThrow("wire-v2 message id");
  });

  it("computes CRC-16/CCITT-FALSE", () => {
    expect(crc16CcittFalse(new TextEncoder().encode("123456789"))).toBe(0x29b1);
  });

  it("builds a single compact frame without rewriting payload bytes", () => {
    const payload = new Uint8Array([1, 2, 3, 4]);
    const frame = buildSingleWireV2Frame({ messageId: 300, payload });

    expect(Array.from(frame.slice(0, 3))).toEqual([2, 172, 2]);
    expect(Array.from(frame.slice(3, -2))).toEqual([1, 2, 3, 4]);
    expect(frame[frame.length - 2]).toBe(0xc3);
    expect(frame[frame.length - 1]).toBe(0x89);
  });
});
