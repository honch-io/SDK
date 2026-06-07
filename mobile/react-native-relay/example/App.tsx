import React, { useEffect, useState } from "react";
import {
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View
} from "react-native";
import type { StoredRelayMessage } from "@honch/react-native-relay";
import { relay } from "./relay";

export default function App() {
  const [status, setStatus] = useState("idle");
  const [pending, setPending] = useState<StoredRelayMessage[]>([]);
  const [lastError, setLastError] = useState<string>("none");
  const [lastReceivedDeviceId, setLastReceivedDeviceId] = useState<string>("none");

  useEffect(() => {
    void refreshPending();
  }, []);

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

  async function drainUploads() {
    await run("draining", () => relay.drainUploads());
  }

  async function receiveHostFrame(
    deviceId: string,
    frameBytes: Uint8Array,
    writeAck: (ackBytes: Uint8Array) => Promise<void>
  ) {
    await relay.receiveFrame(deviceId, frameBytes, {
      acknowledge: async ({ ackBytes }) => {
        await writeAck(ackBytes);
        setLastReceivedDeviceId(deviceId);
      }
    });
    await refreshPending();
  }

  void receiveHostFrame;

  return (
    <SafeAreaView style={styles.root}>
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={styles.title}>Honch Relay Example</Text>
        <View style={styles.section}>
          <Text>Status: {status}</Text>
          <Text>Pending messages: {pending.length}</Text>
          <Text>Last received device: {lastReceivedDeviceId}</Text>
          <Text>Last error: {lastError}</Text>
        </View>
        <View style={styles.actions}>
          <TouchableOpacity style={styles.button} onPress={drainUploads}>
            <Text style={styles.buttonText}>Drain Uploads</Text>
          </TouchableOpacity>
        </View>
        <View style={styles.section}>
          <Text style={styles.heading}>Pending Messages</Text>
          {pending.map((message) => (
            <View key={`${message.deviceId}:${message.sequence}`} style={styles.messageRow}>
              <Text>{message.deviceId}</Text>
              <Text>
                seq {message.sequence} / {message.body.byteLength} bytes
              </Text>
            </View>
          ))}
        </View>
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1
  },
  content: {
    gap: 16,
    padding: 20
  },
  title: {
    fontSize: 24,
    fontWeight: "700"
  },
  section: {
    gap: 8
  },
  heading: {
    fontSize: 16,
    fontWeight: "700"
  },
  actions: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 8
  },
  button: {
    backgroundColor: "#111827",
    borderRadius: 6,
    paddingHorizontal: 12,
    paddingVertical: 10
  },
  buttonText: {
    color: "#ffffff",
    fontWeight: "600"
  },
  messageRow: {
    borderColor: "#d1d5db",
    borderRadius: 6,
    borderWidth: 1,
    gap: 4,
    padding: 12
  }
});
