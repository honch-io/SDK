import { createBleRelayReceiver } from "./ble";
import { drainRelayQueue } from "./drain";
import { subscribeRelayNativeFrames, type RelayNativeFrameEventSource } from "./nativeFrameEvents";
import { createDurableRelayQueue, type StoredRelayMessage } from "./relayQueue";
import { createRelayUploadScheduler } from "./scheduler";
import { uploadRelayMessageOutcome, type RelayUploaderConfig } from "./uploader";
import type { RelayBleNative } from "./ble";
import type { RelayDurableStore } from "./durableStore";
import type { RelayUploadSchedulerNative } from "./scheduler";

export type MobileRelayOptions = {
  durableStore: RelayDurableStore;
  uploaderConfig: RelayUploaderConfig;
  bleNative: RelayBleNative;
  schedulerNative: RelayUploadSchedulerNative;
  frameEvents?: RelayNativeFrameEventSource;
  random?: () => number;
};

export function createMobileRelay(options: MobileRelayOptions) {
  const queue = createDurableRelayQueue(options.durableStore);
  const receiver = createBleRelayReceiver({ queue, native: options.bleNative });
  const scheduler = createRelayUploadScheduler({ native: options.schedulerNative });

  return {
    startScan() {
      return receiver.startScan();
    },

    stopScan() {
      return receiver.stopScan();
    },

    connect(deviceId: string) {
      return receiver.connect(deviceId);
    },

    disconnect(deviceId: string) {
      return receiver.disconnect(deviceId);
    },

    subscribeFrames(deviceId: string) {
      return receiver.subscribeFrames(deviceId);
    },

    receiveFrame(deviceId: string, frameBytes: Uint8Array) {
      return receiver.receiveFrame(deviceId, frameBytes);
    },

    subscribeNativeFrames() {
      if (options.frameEvents === undefined) {
        throw new Error("native frame event source is not configured");
      }
      return subscribeRelayNativeFrames(options.frameEvents, async (deviceId, frameBytes) => {
        await receiver.receiveFrame(deviceId, frameBytes);
      });
    },

    pending(): Promise<StoredRelayMessage[]> {
      return queue.pending();
    },

    drainUploads(): Promise<void> {
      return drainRelayQueue({
        queue,
        upload: (message) => uploadRelayMessageOutcome(options.uploaderConfig, message),
        recordRetry: async (_message, delayMs) => scheduler.schedule(delayMs),
        random: options.random
      });
    }
  };
}
