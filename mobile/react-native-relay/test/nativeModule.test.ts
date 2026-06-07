import { describe, expect, it } from "vitest";

import { createRelayNativeBindings } from "../src/nativeModule";

describe("createRelayNativeBindings", () => {
  it("maps a React Native native module object to scheduler bindings", async () => {
    const calls: string[] = [];
    const bindings = createRelayNativeBindings({
      async scheduleUpload(delayMs: number) {
        calls.push(`schedule:${delayMs}`);
      },
      async cancelUpload() {
        calls.push("cancel");
      }
    });

    await bindings.schedulerNative.scheduleUpload(5000);
    await bindings.schedulerNative.cancelUpload();

    expect(calls).toEqual(["schedule:5000", "cancel"]);
  });

  it("rejects missing native module methods early", () => {
    expect(() => createRelayNativeBindings({})).toThrow("missing native relay method: scheduleUpload");
  });
});
