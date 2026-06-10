import { existsSync, readFileSync } from "node:fs";

import { describe, expect, it } from "vitest";

const packageRoot = new URL("../", import.meta.url);

describe("React Native relay package shape", () => {
  it("keeps TypeScript as the package entrypoint", () => {
    const packageJson = JSON.parse(readFileSync(new URL("package.json", packageRoot), "utf8")) as {
      main?: string;
      "react-native"?: string;
      private?: boolean;
      dependencies?: Record<string, string>;
      peerDependencies?: Record<string, string>;
      peerDependenciesMeta?: Record<string, { optional?: boolean }>;
    };

    expect(packageJson.main).toBe("src/index.ts");
    expect(packageJson["react-native"]).toBe("src/index.ts");
    expect(packageJson.private).not.toBe(true);
    expect(packageJson.peerDependencies?.["react-native"]).toBe(">=0.72");
    expect(packageJson.dependencies?.["react-native-mmkv"]).toBeUndefined();
    expect(packageJson.dependencies?.["react-native-nitro-modules"]).toBeUndefined();
    expect(packageJson.peerDependencies?.["react-native-mmkv"]).toBe(">=2 <5");
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
    expect(packageJson.scripts?.["e2e:capture"]).toBe("bun e2e/relay-capture-e2e.ts");
    expect(existsSync(new URL("scripts/verify-ios-syntax.sh", packageRoot))).toBe(true);
    expect(existsSync(new URL("scripts/verify-android-native.sh", packageRoot))).toBe(true);
  });

  it("includes Android upload scheduler metadata without iOS BLE autolinking", () => {
    const reactNativeConfig = readFileSync(new URL("react-native.config.js", packageRoot), "utf8");
    const expectedFiles = [
      "react-native.config.js",
      "android/build.gradle",
      "android/settings.gradle",
      "android/src/main/AndroidManifest.xml",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayPackage.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadWorker.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadTaskService.java",
      "example/README.md",
      "example/package.json",
      "example/App.tsx",
      "example/relay.ts",
      "example/index.ts"
    ];

    for (const file of expectedFiles) {
      expect(existsSync(new URL(file, packageRoot)), `${file} should exist`).toBe(true);
    }

    expect(reactNativeConfig).not.toContain("ios");
    expect(reactNativeConfig).not.toContain("podspecPath");
    expect(existsSync(new URL("HonchReactNativeRelay.podspec", packageRoot))).toBe(false);
    expect(existsSync(new URL("ios/HonchReactNativeRelay.podspec", packageRoot))).toBe(false);
    expect(existsSync(new URL("ios/HonchReactNativeRelay.m", packageRoot))).toBe(false);
  });

  it("exposes only native upload scheduler methods used by the TypeScript bridge", () => {
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    ) as string;

    for (const method of ["scheduleUpload", "cancelUpload"]) {
      expect(androidModule).toContain(method);
    }

    for (const method of [
      "startScan",
      "stopScan",
      "discoveredDevices",
      "connect",
      "disconnect",
      "subscribeFrames",
      "acknowledgeMessage"
    ]) {
      expect(androidModule).not.toContain(method);
    }
  });

  it("documents iOS upload scheduling as foreground-only without linking a native module", () => {
    const readme = readFileSync(new URL("README.md", packageRoot), "utf8");

    expect(readme).toContain("iOS upload scheduling is foreground-only");
  });

  it("does not merge BLE permissions or receiver code into Android host apps", () => {
    const androidManifest = readFileSync(new URL("android/src/main/AndroidManifest.xml", packageRoot), "utf8");
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    );

    expect(androidManifest).not.toContain("BLUETOOTH_SCAN");
    expect(androidManifest).not.toContain("BLUETOOTH_CONNECT");
    expect(androidManifest).not.toContain('android:usesPermissionFlags="neverForLocation"');
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
      "ByteBuffer.allocate(9)"
    ]) {
      expect(androidModule).not.toContain(expected);
    }
    expect(androidModule).not.toContain("ScanSettings.SCAN_MODE_LOW_LATENCY");
  });

  it("tears down only Android scheduled upload work", () => {
    const androidModule = readFileSync(
      new URL(
        "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
        packageRoot
      ),
      "utf8"
    );

    expect(androidModule).toContain("public void invalidate()");
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

    for (const method of ["scheduleUpload", "cancelUpload"]) {
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
    expect(worker).not.toContain("acquireWakeLock");
    expect(worker).not.toContain("uploadRelayMessage");
    expect(worker).not.toContain("drainUploads");
    expect(service).not.toContain("HeadlessJsTaskService.acquireWakeLockNow(context)");
    expect(service).toContain("private static final long TASK_TIMEOUT_MS = 10000L");
    expect(readme).toContain("Upload retry timing and attempt state stay in the JavaScript relay drain path");
  });

  it("keeps the example relay singleton reusable by foreground UI and headless tasks", () => {
    const relayModule = readFileSync(new URL("example/relay.ts", packageRoot), "utf8");
    const exampleIndex = readFileSync(new URL("example/index.ts", packageRoot), "utf8");

    expect(relayModule).toContain("export const relay");
    expect(relayModule).not.toContain("export const frameEvents");
    expect(exampleIndex).toContain('AppRegistry.registerHeadlessTask("HonchRelayUpload"');
    expect(exampleIndex).toContain("relay.drainUploads()");
  });
});
