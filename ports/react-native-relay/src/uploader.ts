import type { StoredRelayMessage } from "./relayQueue";
import { buildSingleWireV2Frame } from "./wireV2";

export type RelayUploaderConfig = {
  endpointUrl: string;
  projectKey: string;
  relayId: string;
  relaySdkPlatform: string;
  relaySdkVersion: string;
  streamId(message: StoredRelayMessage): string;
  messageId(message: StoredRelayMessage): number;
};

export type RelayUploadOutcome =
  | { action: "consume"; status: number }
  | { action: "drop"; status: number }
  | { action: "retry"; status?: number; error?: unknown };

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
  const frame = buildSingleWireV2Frame({
    messageId: config.messageId(message),
    payload: message.body
  });
  const body = new ArrayBuffer(frame.byteLength);
  new Uint8Array(body).set(frame);

  let response: Response;
  try {
    response = await fetch(`${config.endpointUrl.replace(/\/$/, "")}/capture`, {
      method: "POST",
      headers: {
        "Content-Type": "application/vnd.honch.chunk",
        "X-Honch-Project-Key": config.projectKey,
        "X-Honch-Stream-Id": config.streamId(message),
        "X-Honch-Relay-Id": config.relayId,
        "X-Honch-Relay-SDK-Platform": config.relaySdkPlatform,
        "X-Honch-Relay-SDK-Version": config.relaySdkVersion
      },
      body
    });
  } catch (error) {
    return { action: "retry", error };
  }

  if (response.ok) {
    return { action: "consume", status: response.status };
  }
  if (response.status === 400 || response.status === 401 || response.status === 404) {
    return { action: "drop", status: response.status };
  }
  return { action: "retry", status: response.status };
}
