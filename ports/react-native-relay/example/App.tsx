import React, { useMemo, useState } from "react";
import { NativeModules, SafeAreaView, Text, TouchableOpacity, View } from "react-native";
import {
  createMemoryDurableStore,
  createMobileRelay,
  createRelayNativeBindings,
  type StoredRelayMessage
} from "@honch/react-native-relay";

const captureConfig = {
  endpointUrl: "http://127.0.0.1:8001",
  apiKey: "test_key_123",
  relayId: "mobile-relay-example",
  relaySdkPlatform: "react-native",
  relaySdkVersion: "0.1.0"
};

export default function App() {
  const [status, setStatus] = useState("idle");
  const [pending, setPending] = useState<StoredRelayMessage[]>([]);
  const relay = useMemo(() => {
    const bindings = createRelayNativeBindings(NativeModules.HonchReactNativeRelay);
    return createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig: captureConfig,
      bleNative: bindings.bleNative,
      schedulerNative: bindings.schedulerNative
    });
  }, []);

  async function refreshPending() {
    setPending(await relay.pending());
  }

  async function startScan() {
    setStatus("scanning");
    await relay.startScan();
    await refreshPending();
  }

  async function drainUploads() {
    setStatus("draining");
    await relay.drainUploads();
    await refreshPending();
    setStatus("idle");
  }

  return (
    <SafeAreaView>
      <View>
        <Text>Honch Relay Example</Text>
        <Text>Status: {status}</Text>
        <Text>Pending messages: {pending.length}</Text>
        <TouchableOpacity onPress={startScan}>
          <Text>Start Scan</Text>
        </TouchableOpacity>
        <TouchableOpacity onPress={drainUploads}>
          <Text>Drain Uploads</Text>
        </TouchableOpacity>
      </View>
    </SafeAreaView>
  );
}
