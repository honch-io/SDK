import { drainRelayQueue } from "./drain";
import { createRelayFrameReceiver, type RelayReceiveFrameOptions } from "./relayFrameReceiver";
import { createDurableRelayQueue, type StoredRelayMessage } from "./relayQueue";
import { createRelayUploadScheduler } from "./scheduler";
import { uploadRelayMessageOutcome, type RelayUploaderConfig } from "./uploader";
import type { RelayDurableStore } from "./durableStore";
import type { RelayUploadSchedulerNative } from "./scheduler";

export type MobileRelayOptions = {
  durableStore: RelayDurableStore;
  uploaderConfig: RelayUploaderConfig;
  schedulerNative?: RelayUploadSchedulerNative;
  random?: () => number;
};

export function createMobileRelay(options: MobileRelayOptions) {
  const queue = createDurableRelayQueue(options.durableStore);
  const receiver = createRelayFrameReceiver({ queue });
  const scheduler =
    options.schedulerNative === undefined
      ? undefined
      : createRelayUploadScheduler({ native: options.schedulerNative });

  return {
    receiveFrame(deviceId: string, frameBytes: Uint8Array, receiveOptions?: RelayReceiveFrameOptions) {
      return receiver.receiveFrame(deviceId, frameBytes, receiveOptions);
    },

    pending(): Promise<StoredRelayMessage[]> {
      return queue.pending();
    },

    async startUploadScheduler(): Promise<void> {
      if (scheduler === undefined) {
        await this.drainUploads();
        return;
      }
      await scheduler.schedule(0);
      await this.drainUploads();
    },

    async stopUploadScheduler(): Promise<void> {
      if (scheduler === undefined) {
        return;
      }
      await scheduler.cancel();
    },

    drainUploads(): Promise<void> {
      const drainStartedAtMs = Date.now();
      return drainRelayQueue({
        queue,
        upload: (message) => uploadRelayMessageOutcome(options.uploaderConfig, message),
        now: () => drainStartedAtMs,
        random: options.random
      }).then(async () => {
        const pending = await queue.pending();
        const dueTimes = pending
          .map((message) => message.nextAttemptAtMs)
          .filter((value): value is number => value !== undefined);
        if (dueTimes.length > 0) {
          await scheduler?.schedule(Math.max(0, Math.min(...dueTimes) - drainStartedAtMs));
        }
      });
    }
  };
}
