export interface RelayUploadSchedulerNative {
  scheduleUpload(delayMs: number): Promise<void>;
  cancelUpload(): Promise<void>;
}

export type RelayUploadSchedulerOptions = {
  native: RelayUploadSchedulerNative;
};

export function createRelayUploadScheduler(options: RelayUploadSchedulerOptions) {
  return {
    schedule(delayMs: number): Promise<void> {
      return options.native.scheduleUpload(delayMs);
    },

    cancel(): Promise<void> {
      return options.native.cancelUpload();
    }
  };
}
