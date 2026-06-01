package io.honch.reactnativerelay;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.work.Worker;
import androidx.work.WorkerParameters;

public class HonchRelayUploadWorker extends Worker {
    public static final String WORK_TAG = "honch-relay-upload";
    private static final String TAG = "HonchRelayUpload";

    public HonchRelayUploadWorker(@NonNull Context context, @NonNull WorkerParameters params) {
        super(context, params);
    }

    @NonNull
    @Override
    public Result doWork() {
        Context context = getApplicationContext();
        Intent intent = new Intent(context, HonchRelayUploadTaskService.class);
        intent.putExtra(HonchRelayUploadTaskService.EXTRA_DELAY_MS, getInputData().getLong("delayMs", 0L));

        try {
            context.startService(intent);
            HonchRelayUploadTaskService.acquireWakeLock(context);
            return Result.success();
        } catch (RuntimeException error) {
            Log.e(TAG, "Failed to start relay upload task", error);
            return Result.retry();
        }
    }
}
