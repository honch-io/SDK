import { describe, expect, it } from "vitest";

import { decodeRelayFrame } from "../src/frame";

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
  sourceType?: number;
  first?: boolean;
  final?: boolean;
  sequence?: bigint;
  offset?: number;
  payload?: number[];
}): Uint8Array {
  const payload = new Uint8Array(options.payload ?? []);
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = options.sourceType ?? 1;
  bytes[2] = (options.first ? 1 : 0) | (options.final ? 2 : 0);
  let sequence = options.sequence ?? 1n;
  for (let i = 11; i >= 4; i -= 1) {
    bytes[i] = Number(sequence & 0xffn);
    sequence >>= 8n;
  }
  const offset = options.offset ?? 0;
  bytes[12] = (offset >>> 24) & 0xff;
  bytes[13] = (offset >>> 16) & 0xff;
  bytes[14] = (offset >>> 8) & 0xff;
  bytes[15] = offset & 0xff;
  bytes[16] = (payload.length >>> 8) & 0xff;
  bytes[17] = payload.length & 0xff;
  bytes.set(payload, 20);
  const crc = crc16(new Uint8Array([...bytes.slice(0, 18), ...payload]));
  bytes[18] = (crc >>> 8) & 0xff;
  bytes[19] = crc & 0xff;
  return bytes;
}

function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

describe("decodeRelayFrame", () => {
  it("decodes a valid empty payload final frame", () => {
    const bytes = frame({ first: true, final: true, sequence: 7n });

    const decoded = decodeRelayFrame(bytes);

    expect(decoded.version).toBe(1);
    expect(decoded.sourceType).toBe(1);
    expect(decoded.first).toBe(true);
    expect(decoded.final).toBe(true);
    expect(decoded.sequence).toBe(7n);
    expect(decoded.offset).toBe(0);
    expect(decoded.payload.length).toBe(0);
  });

  it("rejects unsupported versions", () => {
    const bytes = frame({ first: true, final: true });
    bytes[0] = 2;

    expect(() => decodeRelayFrame(bytes)).toThrow("unsupported relay frame version");
  });

  it("rejects unsupported source types", () => {
    const bytes = frame({ sourceType: 2, first: true, final: true });

    expect(() => decodeRelayFrame(bytes)).toThrow("unsupported relay frame source type");
  });

  it("decodes large offsets as unsigned uint32 values", () => {
    const bytes = frame({ first: true, final: true, offset: 0x80000000, payload: [1] });

    expect(decodeRelayFrame(bytes).offset).toBe(0x80000000);
  });

  it("rejects frames with a mismatched crc", () => {
    const bytes = frame({ first: true, final: true, payload: [1, 2, 3] });
    bytes[22] ^= 0xff;

    expect(() => decodeRelayFrame(bytes)).toThrow("relay frame crc mismatch");
  });

  it("matches the relay single chunk conformance fixture", () => {
    const bytes = frame({ first: true, final: true, sequence: 7n, payload: [1, 2, 3] });

    expect(bytesToHex(bytes)).toBe("010103000000000000000007000000000003438b010203");
  });

  it("matches the relay multi chunk conformance fixture", () => {
    const first = frame({ first: true, sequence: 8n, payload: [1, 2] });
    const final = frame({ final: true, sequence: 8n, offset: 2, payload: [3, 4] });

    expect(bytesToHex(first)).toBe("010101000000000000000008000000000002b9c90102");
    expect(bytesToHex(final)).toBe("010102000000000000000008000000020002fb9c0304");
  });
});
