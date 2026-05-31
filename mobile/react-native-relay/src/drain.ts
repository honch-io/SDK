import { nextBackoffDelayMs } from "./retry";
import type { RelayQueue, StoredRelayMessage } from "./relayQueue";
import type { RelayUploadOutcome } from "./uploader";

export type DrainRelayQueueOptions = {
  queue: RelayQueue;
  upload(message: StoredRelayMessage): Promise<RelayUploadOutcome>;
  recordRetry(message: StoredRelayMessage, delayMs: number): Promise<void>;
  random?: () => number;
};

export async function drainRelayQueue(options: DrainRelayQueueOptions): Promise<void> {
  const messages = await options.queue.pending();
  for (const message of messages) {
    const outcome = await options.upload(message);
    if (outcome.action === "consume") {
      await options.queue.markUploaded(message.deviceId, message.sequence);
      continue;
    }
    if (outcome.action === "drop") {
      await options.queue.markDropped(message.deviceId, message.sequence);
      continue;
    }
    await options.recordRetry(message, nextBackoffDelayMs(0, options.random));
  }
}
