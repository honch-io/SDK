import { describe, expect, it } from "vitest";

import { createRelayNativeBindings } from "../src/nativeModule";

describe("createRelayNativeBindings", () => {
  it("maps a React Native native module object to BLE and scheduler bindings", async () => {
    const calls: string[] = [];
    const bindings = createRelayNativeBindings({
      async startScan() {
        calls.push("startScan");
      },
      async stopScan() {
        calls.push("stopScan");
      },
      async discoveredDevices() {
        calls.push("discoveredDevices");
        return [{ id: "device-a", name: "Relay A", rssi: -42 }];
      },
      async connect(deviceId: string) {
        calls.push(`connect:${deviceId}`);
      },
      async subscribeFrames(deviceId: string) {
        calls.push(`subscribeFrames:${deviceId}`);
      },
      async disconnect(deviceId: string) {
        calls.push(`disconnect:${deviceId}`);
      },
      async acknowledgeMessage(deviceId: string, sequence: string) {
        calls.push(`ack:${deviceId}:${sequence}`);
      },
      async scheduleUpload(delayMs: number) {
        calls.push(`schedule:${delayMs}`);
      },
      async cancelUpload() {
        calls.push("cancel");
      }
    });

    await bindings.bleNative.startScan();
    await expect(bindings.bleNative.discoveredDevices()).resolves.toEqual([
      { id: "device-a", name: "Relay A", rssi: -42 }
    ]);
    await bindings.bleNative.connect("device-a");
    await bindings.bleNative.subscribeFrames("device-a");
    await bindings.bleNative.acknowledgeMessage("device-a", "1");
    await bindings.bleNative.disconnect("device-a");
    await bindings.bleNative.stopScan();
    await bindings.schedulerNative.scheduleUpload(5000);
    await bindings.schedulerNative.cancelUpload();

    expect(calls).toEqual([
      "startScan",
      "discoveredDevices",
      "connect:device-a",
      "subscribeFrames:device-a",
      "ack:device-a:1",
      "disconnect:device-a",
      "stopScan",
      "schedule:5000",
      "cancel"
    ]);
  });

  it("rejects missing native module methods early", () => {
    expect(() => createRelayNativeBindings({})).toThrow("missing native relay method: startScan");
  });
});
