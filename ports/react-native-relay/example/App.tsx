import React, { useEffect, useMemo, useState } from "react";
import { NativeEventEmitter, NativeModules, SafeAreaView, Text, TouchableOpacity, View } from "react-native";
import { createMMKV } from "react-native-mmkv";
import {
  RELAY_FRAME_EVENT_NAME,
  createMobileRelay,
  createMmkvRelayStore,
  createRelayNativeBindings,
  type StoredRelayMessage
} from "@honch/react-native-relay";

const captureConfig = {
  endpointUrl: "http://127.0.0.1:8001",
  projectKey: "test_key_123",
  relayId: "mobile-relay-example",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0",
  streamId: (message: StoredRelayMessage) => `relay-${message.deviceId}`,
  messageId: (message: StoredRelayMessage) => Number(message.sequence)
};

export default function App() {
  const [status, setStatus] = useState("idle");
  const [pending, setPending] = useState<StoredRelayMessage[]>([]);
  const [lastError, setLastError] = useState<string>("none");
  const [lastReceivedDeviceId, setLastReceivedDeviceId] = useState<string>("none");
  const { relay, frameEvents } = useMemo(() => {
    const nativeModule = NativeModules.HonchReactNativeRelay;
    const bindings = createRelayNativeBindings(nativeModule);
    const events = new NativeEventEmitter(nativeModule);
    return {
      frameEvents: events,
      relay: createMobileRelay({
        durableStore: createMmkvRelayStore(createMMKV({ id: "honch-relay-example" })),
        uploaderConfig: captureConfig,
        bleNative: bindings.bleNative,
        schedulerNative: bindings.schedulerNative,
        frameEvents: events
      })
    };
  }, []);

  useEffect(() => {
    const relaySubscription = relay.subscribeNativeFrames();
    const statusSubscription = frameEvents.addListener(
      RELAY_FRAME_EVENT_NAME,
      (event: { deviceId?: string }) => {
        setLastReceivedDeviceId(event.deviceId ?? "unknown");
        void refreshPending();
      }
    );
    return () => {
      relaySubscription.remove();
      statusSubscription.remove();
    };
  }, [frameEvents, relay]);

  async function refreshPending() {
    setPending(await relay.pending());
  }

  async function run(label: string, action: () => Promise<void>) {
    setStatus(label);
    setLastError("none");
    try {
      await action();
      await refreshPending();
      setStatus("idle");
    } catch (error) {
      setLastError(error instanceof Error ? error.message : String(error));
      setStatus("error");
    }
  }

  async function startScan() {
    await run("scanning", () => relay.startScan());
  }

  async function stopScan() {
    await run("stopping scan", () => relay.stopScan());
  }

  async function connect() {
    if (lastReceivedDeviceId === "none") {
      setLastError("No relay device has been received yet");
      return;
    }
    await run("connecting", async () => {
      await relay.connect(lastReceivedDeviceId);
      await relay.subscribeFrames(lastReceivedDeviceId);
    });
  }

  async function drainUploads() {
    await run("draining", () => relay.drainUploads());
  }

  return (
    <SafeAreaView>
      <View>
        <Text>Honch Relay Example</Text>
        <Text>Status: {status}</Text>
        <Text>Pending messages: {pending.length}</Text>
        <Text>Last received device: {lastReceivedDeviceId}</Text>
        <Text>Last error: {lastError}</Text>
        <TouchableOpacity onPress={startScan}>
          <Text>Start Scan</Text>
        </TouchableOpacity>
        <TouchableOpacity onPress={stopScan}>
          <Text>Stop Scan</Text>
        </TouchableOpacity>
        <TouchableOpacity onPress={connect}>
          <Text>Connect</Text>
        </TouchableOpacity>
        <TouchableOpacity onPress={drainUploads}>
          <Text>Drain Uploads</Text>
        </TouchableOpacity>
      </View>
    </SafeAreaView>
  );
}
