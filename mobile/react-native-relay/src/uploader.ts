import type { StoredRelayMessage } from "./relayQueue";
import { buildSingleWireV2Frame, buildWireV2Frames, MAX_WIRE_V2_FRAME_SIZE } from "./wireV2";

export type RelayUploaderConfig = {
  endpointUrl: string;
  projectKey: string;
  relayId: string;
  relaySdkPlatform: string;
  relaySdkVersion: string;
  streamId(message: StoredRelayMessage): string;
  messageId(message: StoredRelayMessage): number;
  // Max wire-v2 frame size for re-chunking; defaults to MAX_WIRE_V2_FRAME_SIZE.
  maxFrameSize?: number;
};

export type RelayUploadOutcome =
  | { action: "consume"; status: number }
  | { action: "drop"; status?: number; error?: unknown }
  | { action: "retry"; status?: number; retryAfterMs?: number; error?: unknown };

export function buildRelayUploadBuffer(
  config: Pick<RelayUploaderConfig, "messageId">,
  message: StoredRelayMessage
): ArrayBuffer {
  const frame = buildSingleWireV2Frame({
    messageId: config.messageId(message),
    payload: message.body
  });
  const body = new ArrayBuffer(frame.byteLength);
  new Uint8Array(body).set(frame);
  return body;
}

export async function uploadRelayMessage(
  config: RelayUploaderConfig,
  message: StoredRelayMessage
): Promise<void> {
  const outcome = await uploadRelayMessageOutcome(config, message);
  if (outcome.action === "consume") {
    return;
  }
  if (outcome.status !== undefined) {
    throw new Error(`relay upload failed: ${outcome.status}`);
  }
  throw new Error("relay upload failed: network");
}

export async function uploadRelayMessageOutcome(
  config: RelayUploaderConfig,
  message: StoredRelayMessage
): Promise<RelayUploadOutcome> {
  // Header values flow straight into the HTTP request. Reject control characters
  // (CR/LF/NUL) in the project key and the stream id (derived from the device
  // id) so a crafted value cannot inject headers -- and so an illegal value
  // can't make fetch throw and wedge the message in an endless retry loop.
  const streamId = config.streamId(message);
  if (!isSafeHeaderValue(config.projectKey) || !isSafeHeaderValue(streamId)) {
    return { action: "drop", error: new Error("relay header value contains control characters") };
  }

  // Re-chunk the (possibly oversized) reassembled body into wire-v2 frames and
  // POST them in order: every non-final frame must return 202 (stored, send
  // next), and the final frame returns 204 (complete). Any error mid-sequence
  // ends the attempt; the whole message is retried from the first frame (which
  // also satisfies 409 "retry from offset 0").
  let frames: Uint8Array[];
  try {
    frames = buildWireV2Frames(
      config.messageId(message),
      message.body,
      config.maxFrameSize ?? MAX_WIRE_V2_FRAME_SIZE
    );
  } catch (error) {
    // An un-encodable message id (e.g. beyond the wire-v2 safe-integer range)
    // can never succeed on retry, so drop this one message instead of letting
    // the throw propagate and stall the entire drain.
    return { action: "drop", error };
  }
  const url = `${config.endpointUrl.replace(/\/$/, "")}/capture`;
  const headers = {
    "Content-Type": "application/vnd.honch.chunk",
    "X-Honch-Project-Key": config.projectKey,
    "X-Honch-Stream-Id": streamId,
    "X-Honch-Relay-Id": config.relayId,
    "X-Honch-Relay-SDK-Platform": config.relaySdkPlatform,
    "X-Honch-Relay-SDK-Version": config.relaySdkVersion
  };

  for (let index = 0; index < frames.length; index += 1) {
    const isFinal = index === frames.length - 1;
    const body = new ArrayBuffer(frames[index].byteLength);
    new Uint8Array(body).set(frames[index]);

    let response: Response;
    try {
      response = await fetch(url, { method: "POST", headers, body });
    } catch (error) {
      return { action: "retry", error };
    }

    const outcome = classifyFrameResponse(response, isFinal);
    if (outcome !== "continue") {
      return outcome;
    }
  }

  // Unreachable: the final frame always yields a terminal outcome.
  return { action: "retry" };
}

function classifyFrameResponse(response: Response, isFinal: boolean): RelayUploadOutcome | "continue" {
  if (isFinal) {
    if (response.status === 204) {
      return { action: "consume", status: response.status };
    }
  } else if (response.status === 202) {
    return "continue";
  }
  // Permanent rejections (matching the C SDK status mapping): malformed (400),
  // bad key (401), not found (404), payload too large (413), unsupported content
  // type (415), semantic validation failure (422). 413 is permanent: the relay
  // already re-chunks to a fixed frame size, so retrying the identical bytes can
  // never clear it -- an oversized payload is dropped and logged like the C SDK.
  if (
    response.status === 400 ||
    response.status === 401 ||
    response.status === 404 ||
    response.status === 413 ||
    response.status === 415 ||
    response.status === 422
  ) {
    return { action: "drop", status: response.status };
  }
  // Everything else -- 409 (retry from offset 0), 429, 5xx, and any
  // out-of-sequence 202/204 -- retries the whole message.
  return {
    action: "retry",
    status: response.status,
    retryAfterMs: parseRetryAfterMs(response.headers.get("Retry-After"))
  };
}

function isSafeHeaderValue(value: string): boolean {
  // HTTP header values must not carry control characters; CR/LF in particular
  // would allow header injection, and most fetch implementations throw on them.
  return typeof value === "string" && !/[\x00-\x1f\x7f]/.test(value);
}

function parseRetryAfterMs(value: string | null): number | undefined {
  if (value === null) {
    return undefined;
  }

  const seconds = Number(value);
  if (Number.isFinite(seconds) && seconds >= 0) {
    return Math.round(seconds * 1000);
  }

  const retryAt = Date.parse(value);
  if (!Number.isFinite(retryAt)) {
    return undefined;
  }

  return Math.max(0, retryAt - Date.now());
}
