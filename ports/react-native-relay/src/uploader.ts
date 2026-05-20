import type { StoredRelayMessage } from "./relayQueue";

export type RelayUploaderConfig = {
  endpointUrl: string;
  apiKey: string;
  relayId: string;
  relaySdkPlatform: string;
  relaySdkVersion: string;
};

export async function uploadRelayMessage(
  config: RelayUploaderConfig,
  message: StoredRelayMessage
): Promise<void> {
  const body = new ArrayBuffer(message.body.byteLength);
  new Uint8Array(body).set(message.body);

  const response = await fetch(`${config.endpointUrl.replace(/\/$/, "")}/batch`, {
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
  if (!response.ok) {
    throw new Error(`relay upload failed: ${response.status}`);
  }
}
