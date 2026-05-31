export const RELAY_FRAME_EVENT_NAME = "HonchRelayFrame";

export type RelayNativeFrameEvent = {
  deviceId: string;
  frameBase64: string;
};

export type RelayNativeEventSubscription = {
  remove(): void;
};

export interface RelayNativeFrameEventSource {
  addListener(
    eventName: typeof RELAY_FRAME_EVENT_NAME,
    listener: (event: RelayNativeFrameEvent) => void
  ): RelayNativeEventSubscription;
}

export function subscribeRelayNativeFrames(
  source: RelayNativeFrameEventSource,
  onFrame: (deviceId: string, frameBytes: Uint8Array) => Promise<void> | void
): RelayNativeEventSubscription {
  return source.addListener(RELAY_FRAME_EVENT_NAME, (event) => {
    void onFrame(event.deviceId, decodeRelayFrameEventPayload(event.frameBase64));
  });
}

export function decodeRelayFrameEventPayload(frameBase64: string): Uint8Array {
  const sanitized = frameBase64.replace(/\s/g, "");
  if (sanitized.length % 4 === 1) {
    throw new Error("invalid relay frame base64 payload");
  }

  const bytes: number[] = [];
  for (let offset = 0; offset < sanitized.length; offset += 4) {
    const chunk = sanitized.slice(offset, offset + 4);
    const values = [...chunk].map(decodeBase64Character);
    const first = values[0];
    const second = values[1];
    const third = values[2] ?? 0;
    const fourth = values[3] ?? 0;
    if (first === undefined || second === undefined) {
      throw new Error("invalid relay frame base64 payload");
    }

    bytes.push((first << 2) | (second >> 4));
    if (chunk[2] !== "=" && values.length > 2) {
      bytes.push(((second & 0x0f) << 4) | (third >> 2));
    }
    if (chunk[3] !== "=" && values.length > 3) {
      bytes.push(((third & 0x03) << 6) | fourth);
    }
  }

  return new Uint8Array(bytes);
}

function decodeBase64Character(character: string): number | undefined {
  if (character >= "A" && character <= "Z") {
    return character.charCodeAt(0) - 65;
  }
  if (character >= "a" && character <= "z") {
    return character.charCodeAt(0) - 71;
  }
  if (character >= "0" && character <= "9") {
    return character.charCodeAt(0) + 4;
  }
  if (character === "+") {
    return 62;
  }
  if (character === "/") {
    return 63;
  }
  if (character === "=") {
    return 0;
  }
  return undefined;
}
