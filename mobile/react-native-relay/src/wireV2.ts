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

const HEADER_CONTINUATION = 0x20;
const HEADER_MORE = 0x40;
const FINAL_CRC_SIZE = 2;

// Matches the shipping C SDK frame size (HONCH_WIRE_V2_MAX_FRAME_BYTES), which
// Capture is known to accept, so re-chunked relay frames are guaranteed valid.
export const MAX_WIRE_V2_FRAME_SIZE = 4096;

export function uvarintSize(value: number): number {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error("wire-v2 uvarint must be a non-negative safe integer");
  }
  let size = 1;
  let remaining = Math.floor(value / 128);
  while (remaining !== 0) {
    size += 1;
    remaining = Math.floor(remaining / 128);
  }
  return size;
}

function frameOverhead(
  messageId: number,
  offset: number,
  total: number,
  continuation: boolean,
  more: boolean
): number {
  let overhead = 1 + uvarintSize(messageId);
  if (continuation) {
    overhead += uvarintSize(offset);
  } else if (more) {
    overhead += uvarintSize(total);
  }
  if (!more) {
    overhead += FINAL_CRC_SIZE;
  }
  return overhead;
}

function encodeWireV2Frame(
  messageId: number,
  payload: Uint8Array,
  offset: number,
  total: number,
  continuation: boolean,
  more: boolean,
  crcWholeMessage: number
): Uint8Array {
  const messageIdBytes = encodeUvarint(messageId);
  const positionBytes = continuation
    ? encodeUvarint(offset)
    : more
      ? encodeUvarint(total)
      : new Uint8Array(0);

  let header = (SOURCE_TYPE_EVENTS << 2) | VERSION_CODE;
  if (continuation) header |= HEADER_CONTINUATION;
  if (more) header |= HEADER_MORE;

  const size =
    1 + messageIdBytes.length + positionBytes.length + payload.length + (more ? 0 : FINAL_CRC_SIZE);
  const frame = new Uint8Array(size);
  let p = 0;
  frame[p++] = header;
  frame.set(messageIdBytes, p);
  p += messageIdBytes.length;
  frame.set(positionBytes, p);
  p += positionBytes.length;
  frame.set(payload, p);
  p += payload.length;
  if (!more) {
    // CRC is over the WHOLE reassembled message, not just this final chunk.
    frame[p++] = crcWholeMessage & 0xff;
    frame[p] = (crcWholeMessage >> 8) & 0xff;
  }
  return frame;
}

// Split a complete compact message into wire-v2 frames each <= maxFrameSize,
// so an oversized relayed body is delivered as a multi-frame sequence rather
// than a single over-limit frame. A payload that already fits returns one frame
// (byte-identical to buildSingleWireV2Frame). Faithful port of the C chunker.
export function buildWireV2Frames(
  messageId: number,
  payload: Uint8Array,
  maxFrameSize: number = MAX_WIRE_V2_FRAME_SIZE
): Uint8Array[] {
  if (!Number.isSafeInteger(maxFrameSize) || maxFrameSize < 8) {
    throw new Error("wire-v2 max frame size too small");
  }
  const total = payload.length;
  const crcWhole = crc16CcittFalse(payload);

  if (total === 0) {
    return [encodeWireV2Frame(messageId, payload, 0, 0, false, false, crcWhole)];
  }

  const frames: Uint8Array[] = [];
  let offset = 0;
  do {
    const continuation = offset !== 0;
    const remaining = total - offset;

    const finalOverhead = frameOverhead(messageId, offset, total, continuation, false);
    if (finalOverhead >= maxFrameSize) {
      throw new Error("wire-v2 frame overhead exceeds max frame size");
    }
    const finalCapacity = maxFrameSize - finalOverhead;

    const more = remaining > finalCapacity;
    let payloadCapacity = finalCapacity;
    if (more) {
      const moreOverhead = frameOverhead(messageId, offset, total, continuation, true);
      if (moreOverhead >= maxFrameSize) {
        throw new Error("wire-v2 frame overhead exceeds max frame size");
      }
      payloadCapacity = maxFrameSize - moreOverhead;
    }

    let payloadSize = Math.min(remaining, payloadCapacity);
    if (more && payloadSize >= remaining) {
      payloadSize = remaining - finalCapacity;
    }
    if (payloadSize <= 0) {
      throw new Error("wire-v2 produced a non-positive frame payload");
    }

    frames.push(
      encodeWireV2Frame(
        messageId,
        payload.subarray(offset, offset + payloadSize),
        offset,
        total,
        continuation,
        more,
        crcWhole
      )
    );
    offset += payloadSize;
  } while (offset < total);

  return frames;
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
