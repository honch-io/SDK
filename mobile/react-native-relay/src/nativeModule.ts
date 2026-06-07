import type { RelayUploadSchedulerNative } from "./scheduler";

export type RelayNativeModule = RelayUploadSchedulerNative;

export type RelayNativeBindings = {
  schedulerNative: RelayUploadSchedulerNative;
};

const requiredMethods = [
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
    schedulerNative: {
      scheduleUpload: (delayMs) => typedModule.scheduleUpload(delayMs),
      cancelUpload: () => typedModule.cancelUpload()
    }
  };
}
