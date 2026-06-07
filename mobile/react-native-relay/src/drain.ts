import { nextBackoffDelayMs } from "./retry";
import type { RelayQueue, StoredRelayMessage } from "./relayQueue";
import type { RelayUploadOutcome } from "./uploader";

export type DrainRelayQueueOptions = {
  queue: RelayQueue;
  upload(message: StoredRelayMessage): Promise<RelayUploadOutcome>;
  now?: () => number;
  random?: () => number;
};

export async function drainRelayQueue(options: DrainRelayQueueOptions): Promise<void> {
  const now = options.now ?? Date.now;
  const messages = await options.queue.pending();
  for (const message of messages) {
    const currentTimeMs = now();
    if (message.nextAttemptAtMs !== undefined && message.nextAttemptAtMs > currentTimeMs) {
      continue;
    }
    const outcome = await options.upload(message);
    if (outcome.action === "consume") {
      await options.queue.markUploaded(message.deviceId, message.sequence);
      continue;
    }
    if (outcome.action === "drop") {
      await options.queue.markDropped(message.deviceId, message.sequence);
      continue;
    }
    const attempt = (message.retryAttempt ?? 0) + 1;
    const delayMs = outcome.retryAfterMs ?? nextBackoffDelayMs(message.retryAttempt ?? 0, options.random);
    await options.queue.markRetry(message.deviceId, message.sequence, {
      attempt,
      nextAttemptAtMs: currentTimeMs + delayMs
    });
  }
}
