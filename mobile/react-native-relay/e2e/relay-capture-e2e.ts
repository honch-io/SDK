import { mkdtemp, readFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";

import { createMemoryDurableStore } from "../src/durableStore";
import { createMobileRelay } from "../src/mobileRelay";
import { createJsonFileRelayStore } from "../src/storage/jsonFileStore";

export type RelayCaptureE2EResult = {
  receipt: "PASS";
  retryPreservation: "PASS";
  uploadDrain: "PASS";
  malformedRejection: "PASS";
  clickHouseVerification: "PASS" | "SKIP";
};

export type RelayCaptureE2EOptions = {
  captureUrl: string;
  projectKey: string;
  tempFile?: string;
  relayId?: string;
  fetchImpl?: typeof fetch;
  clickHouseUrl?: string;
};

type WireV2Fixture = {
  frames: Array<{ hex: string; kind: string }>;
};

type RelayFrameOptions = {
  sequence: bigint;
  payload: Uint8Array;
  sourceType?: number;
  first?: boolean;
  final?: boolean;
  offset?: number;
};

export async function runRelayCaptureE2E(options: RelayCaptureE2EOptions): Promise<RelayCaptureE2EResult> {
  await preflightLiveServices(options);

  const tempFile = options.tempFile ?? join(await mkdtemp(join(tmpdir(), "honch-relay-e2e-")), "queue.json");
  const compactMessage = await loadWireV2CompactMessageFixture("single-required-context");
  const firstFrame = buildRelayFrame({
    sequence: 1n,
    first: true,
    final: true,
    payload: compactMessage
  });

  const acknowledgements: string[] = [];
  const relay = createMobileRelay({
    durableStore: createJsonFileRelayStore(tempFile),
    uploaderConfig: {
      endpointUrl: options.captureUrl,
      projectKey: options.projectKey,
      relayId: options.relayId ?? "react-native-relay-e2e",
      relaySdkPlatform: "react-native",
      relaySdkVersion: "0.1.0",
      streamId: (message) => `relay-${message.deviceId}`,
      messageId: (message) => Number(message.sequence)
    },
    bleNative: {
      async startScan() {},
      async stopScan() {},
      async discoveredDevices() {
        return [];
      },
      async connect() {},
      async disconnect() {},
      async subscribeFrames() {},
      async acknowledgeMessage(deviceId, sequence) {
        acknowledgements.push(`${deviceId}:${sequence}`);
      }
    },
    schedulerNative: {
      async scheduleUpload() {},
      async cancelUpload() {}
    },
    random: () => 0.5
  });

  const originalFetch = globalThis.fetch;
  try {
    await relay.receiveFrame("device-a", firstFrame);
    assertEqual(acknowledgements, ["device-a:1"], "relay receipt ACK");
    assertEqual((await relay.pending()).length, 1, "relay receipt pending count");

    globalThis.fetch = async () => Promise.reject(new TypeError("offline"));
    await relay.drainUploads();
    assertEqual((await relay.pending()).length, 1, "offline retry preserves pending message");

    globalThis.fetch = options.fetchImpl ?? originalFetch;
    await relay.drainUploads();
    const afterDrain = await relay.pending();
    if (afterDrain.length !== 0) {
      throw new Error(
        "capture drain did not consume the pending relay message; ensure capture is running, " +
          "the project key exists in Postgres, Pub/Sub is reachable, and capture returns an accepted status"
      );
    }

    const malformedRelay = createMobileRelay({
      durableStore: createMemoryDurableStore(),
      uploaderConfig: {
        endpointUrl: options.captureUrl,
        projectKey: options.projectKey,
        relayId: options.relayId ?? "react-native-relay-e2e",
        relaySdkPlatform: "react-native",
        relaySdkVersion: "0.1.0",
        streamId: () => "malformed",
        messageId: () => 99
      },
      bleNative: {
        async startScan() {},
        async stopScan() {},
        async discoveredDevices() {
          return [];
        },
        async connect() {},
        async disconnect() {},
        async subscribeFrames() {},
        async acknowledgeMessage() {
          throw new Error("malformed frame must not ACK");
        }
      },
      schedulerNative: {
        async scheduleUpload() {},
        async cancelUpload() {}
      }
    });
    const corruptFrame = new Uint8Array(firstFrame);
    corruptFrame[corruptFrame.length - 1] ^= 0xff;
    await expectRejects(() => malformedRelay.receiveFrame("device-a", corruptFrame));
    assertEqual(await malformedRelay.pending(), [], "malformed frame pending queue");

    const clickHouseVerification = options.clickHouseUrl === undefined ? "SKIP" : await verifyClickHouse(options.clickHouseUrl);
    return {
      receipt: "PASS",
      retryPreservation: "PASS",
      uploadDrain: "PASS",
      malformedRejection: "PASS",
      clickHouseVerification
    };
  } finally {
    globalThis.fetch = originalFetch;
  }
}

async function preflightLiveServices(options: RelayCaptureE2EOptions): Promise<void> {
  if (options.fetchImpl === undefined) {
    await preflightHttpEndpoint("capture", `${options.captureUrl.replace(/\/$/, "")}/health`);
  }
  if (options.clickHouseUrl !== undefined) {
    await preflightHttpEndpoint("ClickHouse", `${options.clickHouseUrl.replace(/\/$/, "")}/ping`);
  }
}

async function preflightHttpEndpoint(name: string, url: string): Promise<void> {
  let response: Response;
  try {
    response = await fetch(url);
  } catch (error) {
    throw new Error(`${name} preflight failed: GET ${url} failed: ${String(error)}`);
  }
  if (!response.ok) {
    throw new Error(`${name} preflight failed: GET ${url} returned ${response.status}`);
  }
}

export async function loadWireV2CompactMessageFixture(name: string): Promise<Uint8Array> {
  const fixturePath = new URL(`../../../spec/conformance/wire-v2/${name}.json`, import.meta.url);
  const fixture = JSON.parse(await readFile(fixturePath, "utf8")) as WireV2Fixture;
  const single = fixture.frames.find((frame) => frame.kind === "single");
  if (single === undefined) {
    throw new Error(`wire-v2 fixture ${name} does not contain a single frame`);
  }
  const frame = hexToBytes(single.hex);
  return frame.slice(2, -2);
}

export function buildRelayFrame(options: RelayFrameOptions): Uint8Array {
  const payload = options.payload;
  const bytes = new Uint8Array(20 + payload.length);
  bytes[0] = 1;
  bytes[1] = options.sourceType ?? 1;
  bytes[2] = (options.first ? 1 : 0) | (options.final ? 2 : 0);
  let sequence = options.sequence;
  for (let index = 11; index >= 4; index -= 1) {
    bytes[index] = Number(sequence & 0xffn);
    sequence >>= 8n;
  }
  const offset = options.offset ?? 0;
  bytes[12] = (offset >>> 24) & 0xff;
  bytes[13] = (offset >>> 16) & 0xff;
  bytes[14] = (offset >>> 8) & 0xff;
  bytes[15] = offset & 0xff;
  bytes[16] = (payload.length >>> 8) & 0xff;
  bytes[17] = payload.length & 0xff;
  bytes.set(payload, 20);
  const crc = crc16CcittFalse(new Uint8Array([...bytes.slice(0, 18), ...payload]));
  bytes[18] = (crc >>> 8) & 0xff;
  bytes[19] = crc & 0xff;
  return bytes;
}

function hexToBytes(hex: string): Uint8Array {
  const bytes = new Uint8Array(hex.length / 2);
  for (let index = 0; index < bytes.length; index += 1) {
    bytes[index] = Number.parseInt(hex.slice(index * 2, index * 2 + 2), 16);
  }
  return bytes;
}

function crc16CcittFalse(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

async function verifyClickHouse(clickHouseUrl: string): Promise<"PASS"> {
  const query = "SELECT count() FROM platform.events WHERE event = 'boot' AND device_id = 'device-1'";
  const response = await fetch(`${clickHouseUrl.replace(/\/$/, "")}/?query=${encodeURIComponent(query)}`);
  if (!response.ok) {
    throw new Error(`ClickHouse verification failed: ${response.status}`);
  }
  const text = (await response.text()).trim();
  if (Number(text) < 1) {
    throw new Error("ClickHouse verification found no relay event rows");
  }
  return "PASS";
}

async function expectRejects(action: () => Promise<unknown>): Promise<void> {
  try {
    await action();
  } catch {
    return;
  }
  throw new Error("expected action to reject");
}

function assertEqual<T>(actual: T, expected: T, label: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${label} mismatch: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

if (process.argv[1]?.endsWith("relay-capture-e2e.ts")) {
  const result = await runRelayCaptureE2E({
    captureUrl: process.env.HONCH_CAPTURE_URL ?? "http://127.0.0.1:8001",
    projectKey: process.env.HONCH_PROJECT_KEY ?? "test_key_123",
    clickHouseUrl: process.env.CLICKHOUSE_URL
  });
  for (const [name, status] of Object.entries(result)) {
    console.log(`${name}: ${status}`);
  }
}
