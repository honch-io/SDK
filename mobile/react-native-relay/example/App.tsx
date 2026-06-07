import React, { useEffect, useState } from "react";
import {
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View
} from "react-native";
import {
  RELAY_FRAME_EVENT_NAME,
  requestRelayAndroidPermissions,
  type RelayDiscoveredDevice,
  type StoredRelayMessage
} from "@honch/react-native-relay";
import { frameEvents, relay } from "./relay";

export default function App() {
  const [status, setStatus] = useState("idle");
  const [devices, setDevices] = useState<RelayDiscoveredDevice[]>([]);
  const [selectedDeviceId, setSelectedDeviceId] = useState<string | undefined>();
  const [pending, setPending] = useState<StoredRelayMessage[]>([]);
  const [lastError, setLastError] = useState<string>("none");
  const [lastReceivedDeviceId, setLastReceivedDeviceId] = useState<string>("none");

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

  async function refreshDevices() {
    const discovered = await relay.discoveredDevices();
    setDevices(discovered);
    setSelectedDeviceId((current) => current ?? discovered[0]?.id);
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
    await run("scanning", async () => {
      const permissions = await requestRelayAndroidPermissions();
      if (!permissions.granted) {
        throw new Error(`Missing relay permissions: ${permissions.denied.join(", ")}`);
      }
      await relay.startScan();
      await refreshDevices();
    });
  }

  async function stopScan() {
    await run("stopping scan", () => relay.stopScan());
  }

  async function connect() {
    if (selectedDeviceId === undefined) {
      setLastError("No relay device has been discovered yet");
      return;
    }
    await run("connecting", async () => {
      await relay.connect(selectedDeviceId);
      await relay.subscribeFrames(selectedDeviceId);
    });
  }

  async function drainUploads() {
    await run("draining", () => relay.drainUploads());
  }

  return (
    <SafeAreaView style={styles.root}>
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={styles.title}>Honch Relay Example</Text>
        <View style={styles.section}>
          <Text>Status: {status}</Text>
          <Text>Pending messages: {pending.length}</Text>
          <Text>Discovered devices: {devices.length}</Text>
          <Text>Selected device: {selectedDeviceId ?? "none"}</Text>
          <Text>Last received device: {lastReceivedDeviceId}</Text>
          <Text>Last error: {lastError}</Text>
        </View>
        <View style={styles.actions}>
          <TouchableOpacity style={styles.button} onPress={startScan}>
            <Text style={styles.buttonText}>Start Scan</Text>
          </TouchableOpacity>
          <TouchableOpacity style={styles.button} onPress={refreshDevices}>
            <Text style={styles.buttonText}>Refresh Devices</Text>
          </TouchableOpacity>
          <TouchableOpacity style={styles.button} onPress={stopScan}>
            <Text style={styles.buttonText}>Stop Scan</Text>
          </TouchableOpacity>
          <TouchableOpacity style={styles.button} onPress={connect}>
            <Text style={styles.buttonText}>Connect</Text>
          </TouchableOpacity>
          <TouchableOpacity style={styles.button} onPress={drainUploads}>
            <Text style={styles.buttonText}>Drain Uploads</Text>
          </TouchableOpacity>
        </View>
        <View style={styles.section}>
          <Text style={styles.heading}>Relay Devices</Text>
          {devices.map((device) => (
            <TouchableOpacity
              key={device.id}
              style={[styles.deviceRow, selectedDeviceId === device.id ? styles.selectedDevice : null]}
              onPress={() => setSelectedDeviceId(device.id)}
            >
              <Text style={styles.deviceName}>{device.name ?? "Unnamed relay"}</Text>
              <Text style={styles.deviceMeta}>
                {device.id}
                {device.rssi === undefined ? "" : ` RSSI ${device.rssi}`}
              </Text>
            </TouchableOpacity>
          ))}
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
  deviceRow: {
    borderColor: "#d1d5db",
    borderRadius: 6,
    borderWidth: 1,
    gap: 4,
    padding: 12
  },
  selectedDevice: {
    borderColor: "#2563eb"
  },
  deviceName: {
    fontWeight: "600"
  },
  deviceMeta: {
    color: "#4b5563"
  },
  messageRow: {
    borderColor: "#d1d5db",
    borderRadius: 6,
    borderWidth: 1,
    gap: 4,
    padding: 12
  }
});
