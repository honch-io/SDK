export type RelayFrame = {
  version: number;
  sourceType: number;
  first: boolean;
  final: boolean;
  sequence: bigint;
  offset: number;
  payload: Uint8Array;
};

export function decodeRelayFrame(bytes: Uint8Array): RelayFrame {
  if (bytes.length < 20) {
    throw new Error("relay frame too short");
  }

  const version = bytes[0];
  if (version !== 1) {
    throw new Error("unsupported relay frame version");
  }
  if (bytes[3] !== 0) {
    throw new Error("relay frame reserved byte must be zero");
  }

  const payloadLength = (bytes[16] << 8) | bytes[17];
  if (bytes.length !== 20 + payloadLength) {
    throw new Error("relay frame payload length mismatch");
  }

  let sequence = 0n;
  for (let i = 4; i < 12; i += 1) {
    sequence = (sequence << 8n) | BigInt(bytes[i]);
  }

  const offset = (bytes[12] << 24) | (bytes[13] << 16) | (bytes[14] << 8) | bytes[15];
  return {
    version,
    sourceType: bytes[1],
    first: (bytes[2] & 1) !== 0,
    final: (bytes[2] & 2) !== 0,
    sequence,
    offset,
    payload: bytes.slice(20)
  };
}
