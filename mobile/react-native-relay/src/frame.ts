export type RelayFrame = {
  version: number;
  sourceType: number;
  first: boolean;
  final: boolean;
  sequence: bigint;
  offset: number;
  payload: Uint8Array;
};

const RELAY_FRAME_HEADER_SIZE = 20;
const RELAY_FRAME_CRC_OFFSET = 18;
const RELAY_FRAME_SOURCE_TYPE_EVENTS = 1;
const RELAY_FRAME_CRC_INITIAL = 0xffff;
const RELAY_FRAME_CRC_POLYNOMIAL = 0x1021;

export function decodeRelayFrame(bytes: Uint8Array): RelayFrame {
  if (bytes.length < RELAY_FRAME_HEADER_SIZE) {
    throw new Error("relay frame too short");
  }

  const version = bytes[0];
  if (version !== 1) {
    throw new Error("unsupported relay frame version");
  }
  if (bytes[1] !== RELAY_FRAME_SOURCE_TYPE_EVENTS) {
    throw new Error("unsupported relay frame source type");
  }
  if (bytes[3] !== 0) {
    throw new Error("relay frame reserved byte must be zero");
  }

  const payloadLength = (bytes[16] << 8) | bytes[17];
  if (bytes.length !== RELAY_FRAME_HEADER_SIZE + payloadLength) {
    throw new Error("relay frame payload length mismatch");
  }

  const expectedCrc = readU16(bytes, RELAY_FRAME_CRC_OFFSET);
  const actualCrc = relayFrameCrc16(bytes.slice(0, 18), bytes.slice(RELAY_FRAME_HEADER_SIZE));
  if (actualCrc !== expectedCrc) {
    throw new Error("relay frame crc mismatch");
  }

  let sequence = 0n;
  for (let i = 4; i < 12; i += 1) {
    sequence = (sequence << 8n) | BigInt(bytes[i]);
  }

  const offset = ((bytes[12] << 24) | (bytes[13] << 16) | (bytes[14] << 8) | bytes[15]) >>> 0;
  return {
    version,
    sourceType: bytes[1],
    first: (bytes[2] & 1) !== 0,
    final: (bytes[2] & 2) !== 0,
    sequence,
    offset,
    payload: bytes.slice(RELAY_FRAME_HEADER_SIZE)
  };
}

function readU16(bytes: Uint8Array, offset: number): number {
  return (bytes[offset] << 8) | bytes[offset + 1];
}

function relayFrameCrc16(headerWithoutCrc: Uint8Array, payload: Uint8Array): number {
  let crc = crc16Update(RELAY_FRAME_CRC_INITIAL, headerWithoutCrc);
  crc = crc16Update(crc, payload);
  return crc;
}

function crc16Update(initialCrc: number, bytes: Uint8Array): number {
  let crc = initialCrc;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      if ((crc & 0x8000) !== 0) {
        crc = ((crc << 1) ^ RELAY_FRAME_CRC_POLYNOMIAL) & 0xffff;
      } else {
        crc = (crc << 1) & 0xffff;
      }
    }
  }
  return crc;
}
