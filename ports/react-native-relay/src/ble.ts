import type { RelayQueue, StoredRelayMessage } from "./relayQueue";

export type RelayDiscoveredDevice = {
  id: string;
  name?: string;
  rssi?: number;
};

export interface RelayBleNative {
  startScan(): Promise<void>;
  stopScan(): Promise<void>;
  discoveredDevices(): Promise<RelayDiscoveredDevice[]>;
  connect(deviceId: string): Promise<void>;
  disconnect(deviceId: string): Promise<void>;
  subscribeFrames(deviceId: string): Promise<void>;
  acknowledgeMessage(deviceId: string, sequence: string): Promise<void>;
}

export type RelayBleReceiverOptions = {
  queue: RelayQueue;
  native: RelayBleNative;
};

export type RelayFrameReceipt = {
  complete: boolean;
  message?: StoredRelayMessage;
};

export function createBleRelayReceiver(options: RelayBleReceiverOptions) {
  return {
    startScan() {
      return options.native.startScan();
    },

    stopScan() {
      return options.native.stopScan();
    },

    discoveredDevices() {
      return options.native.discoveredDevices();
    },

    connect(deviceId: string) {
      return options.native.connect(deviceId);
    },

    disconnect(deviceId: string) {
      return options.native.disconnect(deviceId);
    },

    subscribeFrames(deviceId: string) {
      return options.native.subscribeFrames(deviceId);
    },

    async receiveFrame(deviceId: string, frameBytes: Uint8Array): Promise<RelayFrameReceipt> {
      const result = await options.queue.putChunk(deviceId, frameBytes);
      if (result.complete && result.message !== undefined) {
        await options.native.acknowledgeMessage(deviceId, result.message.sequence);
      }
      return result;
    }
  };
}
