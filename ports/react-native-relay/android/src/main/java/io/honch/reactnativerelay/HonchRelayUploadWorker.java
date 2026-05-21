package io.honch.reactnativerelay;

import android.content.Context;

import androidx.annotation.NonNull;
import androidx.work.Worker;
import androidx.work.WorkerParameters;

public class HonchRelayUploadWorker extends Worker {
    public static final String WORK_TAG = "honch-relay-upload";

    public HonchRelayUploadWorker(@NonNull Context context, @NonNull WorkerParameters params) {
        super(context, params);
    }

    @NonNull
    @Override
    public Result doWork() {
        return Result.success();
    }
}
