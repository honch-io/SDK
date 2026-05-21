import type { StoredRelayMessage } from "./relayQueue";

export type RelayUploaderConfig = {
  endpointUrl: string;
  apiKey: string;
  relayId: string;
  relaySdkPlatform: string;
  relaySdkVersion: string;
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
  const body = new ArrayBuffer(message.body.byteLength);
  new Uint8Array(body).set(message.body);

  let response: Response;
  try {
    response = await fetch(`${config.endpointUrl.replace(/\/$/, "")}/batch`, {
      method: "POST",
      headers: {
        "Content-Type": "application/cbor",
        "X-Honch-Relay-Id": config.relayId,
        "X-Honch-Relay-SDK-Platform": config.relaySdkPlatform,
        "X-Honch-Relay-SDK-Version": config.relaySdkVersion,
        Authorization: `Bearer ${config.apiKey}`
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
