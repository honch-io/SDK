import { NativeModules } from "react-native";
import { createMMKV } from "react-native-mmkv";
import {
  createMmkvRelayStore,
  createMobileRelay,
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

const nativeModule = NativeModules.HonchReactNativeRelay;
const bindings = createRelayNativeBindings(nativeModule);

export const relay = createMobileRelay({
  durableStore: createMmkvRelayStore(createMMKV({ id: "honch-relay-example" })),
  uploaderConfig: captureConfig,
  schedulerNative: bindings.schedulerNative
});
