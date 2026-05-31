export type WireV2FrameOptions = {
  messageId: number;
  payload: Uint8Array;
};

const VERSION_CODE = 2;
const SOURCE_TYPE_EVENTS = 0;

export function buildSingleWireV2Frame(options: WireV2FrameOptions): Uint8Array {
  const messageId = encodeUvarint(options.messageId);
  const crc = crc16CcittFalse(options.payload);
  const header = (SOURCE_TYPE_EVENTS << 2) | VERSION_CODE;
  const frame = new Uint8Array(1 + messageId.length + options.payload.length + 2);
  let offset = 0;
  frame[offset++] = header;
  frame.set(messageId, offset);
  offset += messageId.length;
  frame.set(options.payload, offset);
  offset += options.payload.length;
  frame[offset++] = crc & 0xff;
  frame[offset] = (crc >> 8) & 0xff;
  return frame;
}

export function encodeUvarint(value: number): Uint8Array {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error("wire-v2 message id must be a non-negative safe integer");
  }

  const bytes: number[] = [];
  let remaining = value;
  do {
    let byte = remaining & 0x7f;
    remaining = Math.floor(remaining / 128);
    if (remaining !== 0) {
      byte |= 0x80;
    }
    bytes.push(byte);
  } while (remaining !== 0);

  return new Uint8Array(bytes);
}

export function crc16CcittFalse(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}
