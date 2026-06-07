package io.honch.reactnativerelay;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Base64;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.work.ExistingWorkPolicy;
import androidx.work.OneTimeWorkRequest;
import androidx.work.WorkManager;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.modules.core.DeviceEventManagerModule;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;

import java.util.concurrent.TimeUnit;

public class HonchReactNativeRelayModule extends ReactContextBaseJavaModule {
    public static final String NAME = "HonchReactNativeRelay";

    private static final String TAG = "HonchRelay";
    private static final String RELAY_FRAME_EVENT_NAME = "HonchRelayFrame";
    private static final UUID RELAY_SERVICE_UUID = UUID.fromString("484f4e43-482d-5245-4c41-592d53445631");
    private static final UUID FRAME_CHARACTERISTIC_UUID = UUID.fromString("484f4e43-482d-5245-4c41-592d4652414d");
    private static final UUID ACK_CHARACTERISTIC_UUID = UUID.fromString("484f4e43-482d-5245-4c41-592d41434b31");
    private static final UUID CLIENT_CHARACTERISTIC_CONFIG_UUID =
        UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final long DEFAULT_SCAN_TIMEOUT_MS = 30000L;

    private final ReactApplicationContext reactContext;
    private final Handler scanTimeoutHandler = new Handler(Looper.getMainLooper());
    private final Map<String, BluetoothDevice> devicesById = new HashMap<>();
    private final Map<String, RelayDeviceInfo> discoveredDevicesById = new HashMap<>();
    private final Map<String, BluetoothGatt> gattsById = new HashMap<>();
    private final Map<String, BluetoothGattCharacteristic> ackCharacteristicsById = new HashMap<>();
    private BluetoothLeScanner scanner;
    private final Runnable scanTimeoutRunnable = new Runnable() {
        @Override
        public void run() {
            stopScanAfterTimeout();
        }
    };

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            if (device != null) {
                String deviceId = device.getAddress();
                devicesById.put(deviceId, device);
                String name = null;
                try {
                    name = device.getName();
                } catch (SecurityException ignored) {
                }
                discoveredDevicesById.put(deviceId, new RelayDeviceInfo(deviceId, name, result.getRssi()));
                Log.d(TAG, "Discovered relay candidate " + deviceId + " rssi=" + result.getRssi());
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            Log.e(TAG, "BLE scan failed with errorCode=" + errorCode);
        }
    };

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                gatt.discoverServices();
            } else if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
                String deviceId = gatt.getDevice().getAddress();
                gattsById.remove(deviceId);
                ackCharacteristicsById.remove(deviceId);
                gatt.close();
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            subscribeFrameNotifications(gatt);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            emitFrameIfRelayCharacteristic(gatt, characteristic, characteristic.getValue());
        }

        @Override
        public void onCharacteristicChanged(
            BluetoothGatt gatt,
            BluetoothGattCharacteristic characteristic,
            byte[] value
        ) {
            emitFrameIfRelayCharacteristic(gatt, characteristic, value);
        }
    };

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
        try {
            Log.d(TAG, "startScan requested");
            BluetoothAdapter adapter = bluetoothAdapter();
            if (adapter == null || !adapter.isEnabled()) {
                Log.w(TAG, "Bluetooth adapter unavailable or disabled");
                promise.reject("bluetooth_unavailable", "Bluetooth is not enabled");
                return;
            }

            scanner = adapter.getBluetoothLeScanner();
            if (scanner == null) {
                Log.w(TAG, "BLE scanner unavailable");
                promise.reject("scanner_unavailable", "BLE scanner is not available");
                return;
            }

            ScanFilter filter = new ScanFilter.Builder()
                .setServiceUuid(new ParcelUuid(RELAY_SERVICE_UUID))
                .build();
            ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_POWER)
                .build();
            scanner.startScan(Arrays.asList(filter), settings, scanCallback);
            scheduleScanTimeout(DEFAULT_SCAN_TIMEOUT_MS);
            Log.d(TAG, "BLE scan started");
            promise.resolve(null);
        } catch (SecurityException error) {
            Log.e(TAG, "Missing bluetooth permission during scan", error);
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            Log.e(TAG, "Unexpected scan failure", error);
            promise.reject("scan_failed", error);
        }
    }

    @ReactMethod
    public void addListener(String eventName) {
        // Required by NativeEventEmitter on recent React Native versions.
    }

    @ReactMethod
    public void removeListeners(double count) {
        // Required by NativeEventEmitter on recent React Native versions.
    }

    @ReactMethod
    public void stopScan(Promise promise) {
        try {
            if (scanner != null) {
                scanner.stopScan(scanCallback);
                scanner = null;
            }
            cancelScanTimeout();
            promise.resolve(null);
        } catch (SecurityException error) {
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            promise.reject("stop_scan_failed", error);
        }
    }

    @ReactMethod
    public void discoveredDevices(Promise promise) {
        try {
            WritableArray devices = Arguments.createArray();
            for (RelayDeviceInfo device : discoveredDevicesById.values()) {
                devices.pushMap(device.toMap());
            }
            promise.resolve(devices);
        } catch (RuntimeException error) {
            promise.reject("discovered_devices_failed", error);
        }
    }

    @ReactMethod
    public void connect(String deviceId, Promise promise) {
        try {
            BluetoothDevice device = devicesById.get(deviceId);
            if (device == null) {
                BluetoothAdapter adapter = bluetoothAdapter();
                if (adapter != null) {
                    device = adapter.getRemoteDevice(deviceId);
                }
            }
            if (device == null) {
                promise.reject("unknown_device", "No relay BLE device matches the device id");
                return;
            }

            BluetoothGatt gatt = device.connectGatt(reactContext, false, gattCallback);
            gattsById.put(deviceId, gatt);
            promise.resolve(deviceId);
        } catch (IllegalArgumentException error) {
            promise.reject("invalid_device_id", error);
        } catch (SecurityException error) {
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            promise.reject("connect_failed", error);
        }
    }

    @ReactMethod
    public void disconnect(String deviceId, Promise promise) {
        try {
            BluetoothGatt gatt = gattsById.remove(deviceId);
            ackCharacteristicsById.remove(deviceId);
            if (gatt != null) {
                gatt.disconnect();
                gatt.close();
            }
            promise.resolve(deviceId);
        } catch (SecurityException error) {
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            promise.reject("disconnect_failed", error);
        }
    }

    @ReactMethod
    public void subscribeFrames(String deviceId, Promise promise) {
        try {
            BluetoothGatt gatt = gattsById.get(deviceId);
            if (gatt == null) {
                promise.reject("unknown_connection", "No relay BLE connection matches the device id");
                return;
            }
            gatt.discoverServices();
            subscribeFrameNotifications(gatt);
            promise.resolve(deviceId);
        } catch (SecurityException error) {
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            promise.reject("subscribe_frames_failed", error);
        }
    }

    @ReactMethod
    public void acknowledgeMessage(String deviceId, String sequence, Promise promise) {
        try {
            BluetoothGatt gatt = gattsById.get(deviceId);
            BluetoothGattCharacteristic ackCharacteristic = ackCharacteristicsById.get(deviceId);
            if (gatt == null || ackCharacteristic == null) {
                promise.reject("ack_unavailable", "ACK characteristic is not available for the relay device");
                return;
            }

            ByteBuffer ackPayload = ByteBuffer.allocate(9).order(ByteOrder.BIG_ENDIAN);
            ackPayload.put((byte)1);
            ackPayload.putLong(Long.parseUnsignedLong(sequence));
            ackCharacteristic.setValue(ackPayload.array());
            gatt.writeCharacteristic(ackCharacteristic);
            promise.resolve(deviceId + ":" + sequence);
        } catch (NumberFormatException error) {
            promise.reject("invalid_sequence", error);
        } catch (SecurityException error) {
            promise.reject("missing_bluetooth_permission", error);
        } catch (RuntimeException error) {
            promise.reject("acknowledge_failed", error);
        }
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
        teardown();
        super.invalidate();
    }

    private BluetoothAdapter bluetoothAdapter() {
        BluetoothManager manager = (BluetoothManager) reactContext.getSystemService(
            Context.BLUETOOTH_SERVICE
        );
        return manager == null ? null : manager.getAdapter();
    }

    private void teardown() {
        cancelScanTimeout();
        try {
            if (scanner != null) {
                scanner.stopScan(scanCallback);
                scanner = null;
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "Failed to stop BLE scan during teardown", error);
        }

        for (BluetoothGatt gatt : gattsById.values()) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (RuntimeException error) {
                Log.w(TAG, "Failed to close BLE GATT during teardown", error);
            }
        }
        gattsById.clear();
        ackCharacteristicsById.clear();
        devicesById.clear();
        discoveredDevicesById.clear();
        cancelScheduledUploads();
    }

    private void cancelScheduledUploads() {
        WorkManager.getInstance(reactContext).cancelAllWorkByTag(HonchRelayUploadWorker.WORK_TAG);
    }

    private void scheduleScanTimeout(long timeoutMs) {
        cancelScanTimeout();
        scanTimeoutHandler.postDelayed(scanTimeoutRunnable, timeoutMs);
    }

    private void cancelScanTimeout() {
        scanTimeoutHandler.removeCallbacks(scanTimeoutRunnable);
    }

    private void stopScanAfterTimeout() {
        try {
            if (scanner != null) {
                scanner.stopScan(scanCallback);
                scanner = null;
                Log.d(TAG, "BLE scan stopped after idle timeout");
            }
        } catch (SecurityException error) {
            Log.w(TAG, "Missing bluetooth permission while stopping timed-out scan", error);
        } catch (RuntimeException error) {
            Log.w(TAG, "Failed to stop timed-out BLE scan", error);
        }
    }

    private static final class RelayDeviceInfo {
        private final String id;
        private final String name;
        private final int rssi;

        RelayDeviceInfo(String id, String name, int rssi) {
            this.id = id;
            this.name = name;
            this.rssi = rssi;
        }

        WritableMap toMap() {
            WritableMap map = Arguments.createMap();
            map.putString("id", id);
            if (name != null) {
                map.putString("name", name);
            }
            map.putInt("rssi", rssi);
            return map;
        }
    }

    private void subscribeFrameNotifications(BluetoothGatt gatt) {
        BluetoothGattService service = gatt.getService(RELAY_SERVICE_UUID);
        if (service == null) {
            return;
        }

        BluetoothGattCharacteristic frameCharacteristic = service.getCharacteristic(FRAME_CHARACTERISTIC_UUID);
        BluetoothGattCharacteristic ackCharacteristic = service.getCharacteristic(ACK_CHARACTERISTIC_UUID);
        String deviceId = gatt.getDevice().getAddress();
        if (ackCharacteristic != null) {
            ackCharacteristicsById.put(deviceId, ackCharacteristic);
        }
        if (frameCharacteristic == null) {
            return;
        }

        gatt.setCharacteristicNotification(frameCharacteristic, true);
        BluetoothGattDescriptor descriptor =
            frameCharacteristic.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID);
        if (descriptor != null) {
            descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            gatt.writeDescriptor(descriptor);
        }
    }

    private void emitFrameIfRelayCharacteristic(
        BluetoothGatt gatt,
        BluetoothGattCharacteristic characteristic,
        byte[] value
    ) {
        if (!FRAME_CHARACTERISTIC_UUID.equals(characteristic.getUuid()) || value == null) {
            return;
        }

        WritableMap payload = Arguments.createMap();
        payload.putString("deviceId", gatt.getDevice().getAddress());
        payload.putString("frameBase64", Base64.encodeToString(value, Base64.NO_WRAP));
        reactContext
            .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
            .emit(RELAY_FRAME_EVENT_NAME, payload);
    }
}
