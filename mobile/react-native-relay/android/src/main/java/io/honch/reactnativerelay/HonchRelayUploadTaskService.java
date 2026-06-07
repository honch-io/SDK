package io.honch.reactnativerelay;

import android.content.Intent;

import androidx.annotation.Nullable;

import com.facebook.react.HeadlessJsTaskService;
import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.jstasks.HeadlessJsTaskConfig;

public class HonchRelayUploadTaskService extends HeadlessJsTaskService {
    public static final String TASK_NAME = "HonchRelayUpload";
    public static final String EXTRA_DELAY_MS = "delayMs";
    private static final long TASK_TIMEOUT_MS = 10000L;

    @Nullable
    @Override
    protected HeadlessJsTaskConfig getTaskConfig(Intent intent) {
        WritableMap data = Arguments.createMap();
        data.putDouble(EXTRA_DELAY_MS, intent.getLongExtra(EXTRA_DELAY_MS, 0L));
        return new HeadlessJsTaskConfig(TASK_NAME, data, TASK_TIMEOUT_MS, true);
    }
}
