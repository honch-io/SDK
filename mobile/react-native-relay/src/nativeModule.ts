import type { RelayBleNative } from "./ble";
import type { RelayUploadSchedulerNative } from "./scheduler";

export type RelayNativeModule = RelayBleNative & RelayUploadSchedulerNative;

export type RelayNativeBindings = {
  bleNative: RelayBleNative;
  schedulerNative: RelayUploadSchedulerNative;
};

const requiredMethods = [
  "startScan",
  "stopScan",
  "discoveredDevices",
  "connect",
  "disconnect",
  "subscribeFrames",
  "acknowledgeMessage",
  "scheduleUpload",
  "cancelUpload"
] as const;

export function createRelayNativeBindings(nativeModule: unknown): RelayNativeBindings {
  const module = nativeModule as Partial<Record<(typeof requiredMethods)[number], unknown>>;
  for (const method of requiredMethods) {
    if (typeof module[method] !== "function") {
      throw new Error(`missing native relay method: ${method}`);
    }
  }

  const typedModule = nativeModule as RelayNativeModule;
  return {
    bleNative: {
      startScan: (options) => typedModule.startScan(options),
      stopScan: () => typedModule.stopScan(),
      discoveredDevices: () => typedModule.discoveredDevices(),
      connect: (deviceId) => typedModule.connect(deviceId),
      disconnect: (deviceId) => typedModule.disconnect(deviceId),
      subscribeFrames: (deviceId) => typedModule.subscribeFrames(deviceId),
      acknowledgeMessage: (deviceId, sequence) =>
        typedModule.acknowledgeMessage(deviceId, sequence)
    },
    schedulerNative: {
      scheduleUpload: (delayMs) => typedModule.scheduleUpload(delayMs),
      cancelUpload: () => typedModule.cancelUpload()
    }
  };
}
