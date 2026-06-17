import { describe, expect, it } from "vitest";

import {
  buildSingleWireV2Frame,
  buildWireV2Frames,
  crc16CcittFalse,
  encodeUvarint,
  uvarintSize
} from "../src/wireV2";

function decodeUvarint(bytes: Uint8Array, start: number): { value: number; next: number } {
  let value = 0;
  let shift = 1;
  let pos = start;
  for (;;) {
    const byte = bytes[pos++];
    value += (byte & 0x7f) * shift;
    if ((byte & 0x80) === 0) break;
    shift *= 128;
  }
  return { value, next: pos };
}

// Decode a frame and reassemble a multi-frame sequence back into the original
// message, validating the structural contract (flags, contiguous offsets,
// per-frame size, whole-message CRC on the final frame).
function reassemble(frames: Uint8Array[], maxFrameSize: number): { message: Uint8Array; messageId: number } {
  let expectedOffset = 0;
  let total: number | undefined;
  let messageId: number | undefined;
  const parts: number[] = [];

  frames.forEach((frame, index) => {
    expect(frame.length).toBeLessThanOrEqual(maxFrameSize);
    const isFinal = index === frames.length - 1;
    let p = 0;
    const header = frame[p++];
    expect(header & 0x03).toBe(2); // version
    const continuation = (header & 0x20) !== 0;
    const more = (header & 0x40) !== 0;
    expect(more).toBe(!isFinal);
    expect(continuation).toBe(index !== 0);

    const mid = decodeUvarint(frame, p);
    p = mid.next;
    if (messageId === undefined) messageId = mid.value;
    expect(mid.value).toBe(messageId);

    if (continuation) {
      const off = decodeUvarint(frame, p);
      p = off.next;
      expect(off.value).toBe(expectedOffset);
    } else if (more) {
      const tot = decodeUvarint(frame, p);
      p = tot.next;
      total = tot.value;
    }

    const payloadEnd = more ? frame.length : frame.length - 2;
    for (let i = p; i < payloadEnd; i += 1) parts.push(frame[i]);
    expectedOffset += payloadEnd - p;

    if (!more) {
      const message = new Uint8Array(parts);
      const crc = frame[frame.length - 2] | (frame[frame.length - 1] << 8);
      expect(crc).toBe(crc16CcittFalse(message)); // CRC is over the WHOLE message
      if (total !== undefined) expect(total).toBe(message.length);
    }
  });

  return { message: new Uint8Array(parts), messageId: messageId as number };
}

describe("wire-v2 multi-frame chunking", () => {
  it("sizes uvarints like LEB128", () => {
    expect(uvarintSize(0)).toBe(1);
    expect(uvarintSize(127)).toBe(1);
    expect(uvarintSize(128)).toBe(2);
    expect(uvarintSize(16_384)).toBe(3);
  });

  it("returns one frame, identical to the single-frame builder, when it fits", () => {
    const payload = new Uint8Array([1, 2, 3, 4, 5]);
    const frames = buildWireV2Frames(300, payload, 4096);
    expect(frames).toHaveLength(1);
    expect(Array.from(frames[0])).toEqual(Array.from(buildSingleWireV2Frame({ messageId: 300, payload })));
  });

  it("splits an oversized body into contiguous frames that reassemble exactly", () => {
    const maxFrameSize = 256;
    const payload = new Uint8Array(5000);
    for (let i = 0; i < payload.length; i += 1) payload[i] = (i * 31 + 7) & 0xff;

    const frames = buildWireV2Frames(70_000, payload, maxFrameSize);
    expect(frames.length).toBeGreaterThan(1);

    const { message, messageId } = reassemble(frames, maxFrameSize);
    expect(messageId).toBe(70_000);
    expect(Array.from(message)).toEqual(Array.from(payload));
  });

  it("uses the default 4096 frame size", () => {
    const payload = new Uint8Array(20_000).fill(0xab);
    const frames = buildWireV2Frames(1, payload);
    expect(frames.length).toBeGreaterThan(1);
    const { message } = reassemble(frames, 4096);
    expect(Array.from(message)).toEqual(Array.from(payload));
  });
});

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
