import { existsSync, readFileSync } from "node:fs";

import { describe, expect, it } from "vitest";

const packageRoot = new URL("../", import.meta.url);

describe("React Native relay package shape", () => {
  it("keeps TypeScript as the package entrypoint", () => {
    const packageJson = JSON.parse(readFileSync(new URL("package.json", packageRoot), "utf8")) as {
      main?: string;
      "react-native"?: string;
    };

    expect(packageJson.main).toBe("src/index.ts");
    expect(packageJson["react-native"]).toBe("src/index.ts");
  });

  it("includes native package metadata for Android and iOS", () => {
    const expectedFiles = [
      "react-native.config.js",
      "android/build.gradle",
      "android/src/main/AndroidManifest.xml",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayModule.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchReactNativeRelayPackage.java",
      "android/src/main/java/io/honch/reactnativerelay/HonchRelayUploadWorker.java",
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
});
