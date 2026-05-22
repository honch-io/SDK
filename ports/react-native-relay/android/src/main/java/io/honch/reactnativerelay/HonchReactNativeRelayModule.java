package io.honch.reactnativerelay;

import androidx.annotation.NonNull;
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
    public void startScan(Promise promise) {
        promise.resolve(null);
    }

    @ReactMethod
    public void stopScan(Promise promise) {
        promise.resolve(null);
    }

    @ReactMethod
    public void connect(String deviceId, Promise promise) {
        promise.resolve(deviceId);
    }

    @ReactMethod
    public void disconnect(String deviceId, Promise promise) {
        promise.resolve(deviceId);
    }

    @ReactMethod
    public void subscribeFrames(String deviceId, Promise promise) {
        promise.resolve(deviceId);
    }

    @ReactMethod
    public void acknowledgeMessage(String deviceId, String sequence, Promise promise) {
        promise.resolve(deviceId + ":" + sequence);
    }

    @ReactMethod
    public void scheduleUpload(double delayMs, Promise promise) {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(HonchRelayUploadWorker.class)
            .setInitialDelay((long)delayMs, TimeUnit.MILLISECONDS)
            .addTag(HonchRelayUploadWorker.WORK_TAG)
            .build();
        WorkManager.getInstance(reactContext).enqueue(request);
        promise.resolve(null);
    }

    @ReactMethod
    public void cancelUpload(Promise promise) {
        WorkManager.getInstance(reactContext).cancelAllWorkByTag(HonchRelayUploadWorker.WORK_TAG);
        promise.resolve(null);
    }
}
