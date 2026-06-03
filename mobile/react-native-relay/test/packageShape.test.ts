import { existsSync, readFileSync } from "node:fs";

import { describe, expect, it } from "vitest";

const packageRoot = new URL("../", import.meta.url);

describe("React Native relay package shape", () => {
  it("keeps TypeScript as the package entrypoint", () => {
    const packageJson = JSON.parse(readFileSync(new URL("package.json", packageRoot), "utf8")) as {
      main?: string;
      "react-native"?: string;
      dependencies?: Record<string, string>;
      peerDependencies?: Record<string, string>;
      peerDependenciesMeta?: Record<string, { optional?: boolean }>;
    };

    expect(packageJson.main).toBe("src/index.ts");
    expect(packageJson["react-native"]).toBe("src/index.ts");
    expect(packageJson.peerDependencies?.["react-native"]).toBe(">=0.72");
    expect(packageJson.dependencies?.["react-native-mmkv"]).toBeUndefined();
    expect(packageJson.dependencies?.["react-native-nitro-modules"]).toBeUndefined();
    expect(packageJson.peerDependencies?.["react-native-mmkv"]).toBe("^4.3.1");
    expect(packageJson.peerDependencies?.["react-native-nitro-modules"]).toBe("^0.35.7");
    expect(packageJson.peerDependenciesMeta?.["react-native-mmkv"]?.optional).toBe(true);
    expect(packageJson.peerDependenciesMeta?.["react-native-nitro-modules"]?.optional).toBe(true);
  });

  it("routes native verification through package-owned scripts", () => {
    const packageJson = JSON.parse(readFileSync(new URL("package.json", packageRoot), "utf8")) as {
      scripts?: Record<string, string>;
    };

    expect(packageJson.scripts?.["verify:ios:native"]).toBe("scripts/verify-ios-syntax.sh");
    expect(packageJson.scripts?.["verify:android:native"]).toBe("scripts/verify-android-native.sh");
    expect(existsSync(new URL("scripts/verify-ios-syntax.sh", packageRoot))).toBe(true);
    expect(existsSync(new URL("scripts/verify-android-native.sh", packageRoot))).toBe(true);
  });

  it("includes native package metadata for Android and iOS", () => {
    const expectedFiles = [
      "react-native.config.js",
      "android/build.gradle",
      "android/settings.gradle",
      "android/src/main/AndroidManifest.xml",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayPackage.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadWorker.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadTaskService.java",
      "ios/HonchReactNativeRelay.podspec",
      "ios/HonchReactNativeRelay.h",
      "ios/HonchReactNativeRelay.m",
      "example/README.md",
      "example/package.json",
      "example/App.tsx"
    ];

    for (const file of expectedFiles) {
      expect(existsSync(new URL(file, packageRoot)), `${file} should exist`).toBe(true);
    }
  });

  it("exposes the native module methods used by the TypeScript bridge", () => {
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    ) as string;
    const iosModule = readFileSync(new URL("ios/HonchReactNativeRelay.m", packageRoot), "utf8") as string;

    for (const method of [
      "startScan",
      "stopScan",
      "discoveredDevices",
      "connect",
      "disconnect",
      "subscribeFrames",
      "acknowledgeMessage",
      "scheduleUpload",
      "cancelUpload"
    ]) {
      expect(androidModule).toContain(method);
      expect(iosModule).toContain(method);
    }
  });

  it("implements the iOS CoreBluetooth relay receiver contract", () => {
    const iosHeader = readFileSync(new URL("ios/HonchReactNativeRelay.h", packageRoot), "utf8");
    const iosModule = readFileSync(new URL("ios/HonchReactNativeRelay.m", packageRoot), "utf8");

    expect(iosHeader).toContain("<CoreBluetooth/CoreBluetooth.h>");
    expect(iosHeader).toContain("RCTEventEmitter");
    expect(iosHeader).toContain("CBCentralManagerDelegate");
    expect(iosHeader).toContain("CBPeripheralDelegate");

    for (const expected of [
      "484f4e43-482d-5245-4c41-592d53445631",
      "484f4e43-482d-5245-4c41-592d4652414d",
      "484f4e43-482d-5245-4c41-592d41434b31",
      "HonchRelayFrame",
      "scanForPeripheralsWithServices",
      "connectPeripheral",
      "setNotifyValue:YES",
      "writeValue:ackData",
      "CBCharacteristicWriteWithResponse"
    ]) {
      expect(iosModule).toContain(expected);
    }
  });

  it("documents iOS upload scheduling as foreground-only instead of linking dead background tasks", () => {
    const podspec = readFileSync(new URL("ios/HonchReactNativeRelay.podspec", packageRoot), "utf8");
    const iosModule = readFileSync(new URL("ios/HonchReactNativeRelay.m", packageRoot), "utf8");
    const readme = readFileSync(new URL("README.md", packageRoot), "utf8");

    expect(podspec).not.toContain("BackgroundTasks");
    expect(iosModule).toContain("reject(");
    expect(iosModule).toContain('"ios_background_upload_unsupported"');
    expect(readme).toContain("iOS upload scheduling is foreground-only");
  });

  it("keeps iOS BLE callbacks and frame encoding off the main queue", () => {
    const iosModule = readFileSync(new URL("ios/HonchReactNativeRelay.m", packageRoot), "utf8");

    expect(iosModule).toContain('dispatch_queue_create("io.honch.relay.ble", DISPATCH_QUEUE_SERIAL)');
    expect(iosModule).toContain("- (dispatch_queue_t)methodQueue");
    expect(iosModule).toContain("initWithDelegate:self queue:_bleQueue");
    expect(iosModule).not.toContain("initWithDelegate:self queue:nil");
  });

  it("tears down iOS scans, peripherals, listeners, and native state", () => {
    const iosModule = readFileSync(new URL("ios/HonchReactNativeRelay.m", packageRoot), "utf8");

    expect(iosModule).toContain("- (void)invalidate");
    expect(iosModule).toContain("- (void)dealloc");
    expect(iosModule).toContain("- (void)teardown");
    expect(iosModule).toContain("[_centralManager stopScan]");
    expect(iosModule).toContain("cancelPeripheralConnection:peripheral");
    expect(iosModule).toContain("peripheral.delegate = nil");
    expect(iosModule).toContain("[self->_peripheralsById removeAllObjects]");
    expect(iosModule).toContain("[self->_ackCharacteristicsById removeAllObjects]");
    expect(iosModule).toContain("_hasListeners = NO");
  });

  it("implements the Android BLE relay receiver contract", () => {
    const androidManifest = readFileSync(new URL("android/src/main/AndroidManifest.xml", packageRoot), "utf8");
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    );

    for (const permission of ["BLUETOOTH_SCAN", "BLUETOOTH_CONNECT"]) {
      expect(androidManifest).toContain(permission);
    }
    expect(androidManifest).toContain('android:usesPermissionFlags="neverForLocation"');
    expect(androidManifest).not.toContain("ACCESS_FINE_LOCATION");
    expect(androidManifest).not.toContain("POST_NOTIFICATIONS");

    for (const expected of [
      "BluetoothManager",
      "BluetoothLeScanner",
      "ScanCallback",
      "BluetoothGattCallback",
      "BluetoothGattCharacteristic",
      "DeviceEventManagerModule.RCTDeviceEventEmitter",
      "HonchRelayFrame",
      "484f4e43-482d-5245-4c41-592d53445631",
      "484f4e43-482d-5245-4c41-592d4652414d",
      "484f4e43-482d-5245-4c41-592d41434b31",
      "setCharacteristicNotification",
      "writeCharacteristic",
      "ByteBuffer.allocate(9)",
      "putLong"
    ]) {
      expect(androidModule).toContain(expected);
    }
    expect(androidModule).toContain("ScanSettings.SCAN_MODE_LOW_POWER");
    expect(androidModule).not.toContain("ScanSettings.SCAN_MODE_LOW_LATENCY");
  });

  it("tears down Android scans, GATT connections, and scheduled upload work", () => {
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    );

    expect(androidModule).toContain("public void invalidate()");
    expect(androidModule).toContain("private void teardown()");
    expect(androidModule).toContain("scanner.stopScan(scanCallback)");
    expect(androidModule).toContain("gatt.disconnect()");
    expect(androidModule).toContain("gatt.close()");
    expect(androidModule).toContain("gattsById.clear()");
    expect(androidModule).toContain("ackCharacteristicsById.clear()");
    expect(androidModule).toContain("cancelAllWorkByTag(HonchRelayUploadWorker.WORK_TAG)");
  });

  it("keeps Android ReactMethod native exceptions contained in promise rejections", () => {
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    );

    for (const method of [
      "stopScan",
      "discoveredDevices",
      "connect",
      "disconnect",
      "subscribeFrames",
      "acknowledgeMessage",
      "scheduleUpload",
      "cancelUpload"
    ]) {
      const methodIndex = androidModule.indexOf(`public void ${method}`);
      const nextMethodIndex = androidModule.indexOf("@ReactMethod", methodIndex + 1);
      const methodBody = androidModule.slice(
        methodIndex,
        nextMethodIndex === -1 ? androidModule.length : nextMethodIndex
      );
      expect(methodBody, `${method} should catch RuntimeException`).toContain("catch (RuntimeException error)");
      expect(methodBody, `${method} should reject promise on native errors`).toContain("promise.reject(");
    }
  });

  it("starts a headless JS upload task from the Android upload worker", () => {
    const androidManifest = readFileSync(new URL("android/src/main/AndroidManifest.xml", packageRoot), "utf8");
    const worker = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadWorker.java",
        packageRoot
      ),
      "utf8"
    );
    const service = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadTaskService.java",
        packageRoot
      ),
      "utf8"
    );

    expect(androidManifest).toContain("HonchRelayUploadTaskService");
    expect(worker).toContain("HonchRelayUploadTaskService");
    expect(worker).toContain("startService");
    expect(worker).toContain("Result.retry()");
    expect(service).toContain("HeadlessJsTaskService");
    expect(service).toContain("HonchRelayUpload");
    expect(
      readFileSync(
        new URL(
          "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
          packageRoot
        ),
        "utf8"
      )
    ).toContain("enqueueUniqueWork");
  });

  it("keeps Android upload retry timing in JS and bounds the headless wake lock", () => {
    const worker = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadWorker.java",
        packageRoot
      ),
      "utf8"
    );
    const service = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadTaskService.java",
        packageRoot
      ),
      "utf8"
    );
    const readme = readFileSync(new URL("README.md", packageRoot), "utf8");

    expect(worker).toContain("return Result.success()");
    expect(worker).toContain("return Result.retry()");
    expect(worker).not.toContain("uploadRelayMessage");
    expect(worker).not.toContain("drainUploads");
    expect(service).toContain("private static final long TASK_TIMEOUT_MS = 10000L");
    expect(readme).toContain("Upload retry timing stays in the JavaScript relay drain path");
  });
});
