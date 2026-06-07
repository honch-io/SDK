package io.honch.reactnativerelay;

import androidx.annotation.NonNull;
import androidx.work.ExistingWorkPolicy;
import androidx.work.OneTimeWorkRequest;
import androidx.work.WorkManager;

import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;

import java.util.concurrent.TimeUnit;

public class HonchReactNativeRelayModule extends ReactContextBaseJavaModule {
    public static final String NAME = "HonchReactNativeRelay";

    private final ReactApplicationContext reactContext;

    public HonchReactNativeRelayModule(ReactApplicationContext reactContext) {
        super(reactContext);
        this.reactContext = reactContext;
    }

    @NonNull
    @Override
    public String getName() {
        return NAME;
    }

    @ReactMethod
    public void scheduleUpload(double delayMs, Promise promise) {
        try {
            OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(HonchRelayUploadWorker.class)
                .setInitialDelay((long)delayMs, TimeUnit.MILLISECONDS)
                .setInputData(new androidx.work.Data.Builder().putLong("delayMs", (long)delayMs).build())
                .addTag(HonchRelayUploadWorker.WORK_TAG)
                .build();
            WorkManager
                .getInstance(reactContext)
                .enqueueUniqueWork(HonchRelayUploadWorker.WORK_TAG, ExistingWorkPolicy.REPLACE, request);
            promise.resolve(null);
        } catch (RuntimeException error) {
            promise.reject("schedule_upload_failed", error);
        }
    }

    @ReactMethod
    public void cancelUpload(Promise promise) {
        try {
            cancelScheduledUploads();
            promise.resolve(null);
        } catch (RuntimeException error) {
            promise.reject("cancel_upload_failed", error);
        }
    }

    @Override
    public void invalidate() {
        cancelScheduledUploads();
        super.invalidate();
    }

    private void cancelScheduledUploads() {
        WorkManager.getInstance(reactContext).cancelAllWorkByTag(HonchRelayUploadWorker.WORK_TAG);
    }
}
