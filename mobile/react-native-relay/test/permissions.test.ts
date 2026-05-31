import { describe, expect, it } from "vitest";

import { requestRelayAndroidPermissions } from "../src/permissions";

describe("requestRelayAndroidPermissions", () => {
  it("is a no-op outside Android", async () => {
    await expect(
      requestRelayAndroidPermissions({
        platformOS: "ios",
        permissions: {
          async requestMultiple() {
            throw new Error("should not request permissions");
          }
        }
      })
    ).resolves.toEqual({ granted: true, requested: [], denied: [] });
  });

  it("requests the BLE relay permission set on Android", async () => {
    const requestedByNative: string[][] = [];

    const result = await requestRelayAndroidPermissions({
      platformOS: "android",
      permissions: {
        PERMISSIONS: {
          BLUETOOTH_SCAN: "android.permission.BLUETOOTH_SCAN",
          BLUETOOTH_CONNECT: "android.permission.BLUETOOTH_CONNECT",
          ACCESS_FINE_LOCATION: "android.permission.ACCESS_FINE_LOCATION"
        },
        RESULTS: {
          GRANTED: "granted"
        },
        async requestMultiple(permissions) {
          requestedByNative.push(permissions);
          return Object.fromEntries(permissions.map((permission) => [permission, "granted"]));
        }
      }
    });

    expect(requestedByNative).toEqual([
      [
        "android.permission.BLUETOOTH_SCAN",
        "android.permission.BLUETOOTH_CONNECT",
        "android.permission.ACCESS_FINE_LOCATION"
      ]
    ]);
    expect(result).toEqual({
      granted: true,
      requested: requestedByNative[0],
      denied: []
    });
  });

  it("reports denied permissions", async () => {
    await expect(
      requestRelayAndroidPermissions({
        platformOS: "android",
        permissions: {
          RESULTS: {
            GRANTED: "granted"
          },
          async requestMultiple(permissions) {
            return Object.fromEntries(
              permissions.map((permission) => [
                permission,
                permission.endsWith("BLUETOOTH_CONNECT") ? "denied" : "granted"
              ])
            );
          }
        }
      })
    ).resolves.toEqual({
      granted: false,
      requested: [
        "android.permission.BLUETOOTH_SCAN",
        "android.permission.BLUETOOTH_CONNECT",
        "android.permission.ACCESS_FINE_LOCATION"
      ],
      denied: ["android.permission.BLUETOOTH_CONNECT"]
    });
  });
});
