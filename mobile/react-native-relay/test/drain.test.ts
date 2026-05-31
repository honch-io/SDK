import { describe, expect, it } from "vitest";

import { drainRelayQueue } from "../src/drain";
import type { RelayQueue, StoredRelayMessage } from "../src/relayQueue";
import type { RelayUploadOutcome } from "../src/uploader";

function message(sequence: string): StoredRelayMessage {
  return {
    deviceId: "device-a",
    sourceType: 1,
    sequence,
    body: new Uint8Array([Number(sequence)])
  };
}

function queue(messages: StoredRelayMessage[]): RelayQueue & {
  uploaded: string[];
  dropped: string[];
} {
  return {
    uploaded: [],
    dropped: [],
    async putChunk() {
      return { complete: false };
    },
    async pending() {
      return messages;
    },
    async markUploaded(_deviceId, sequence) {
      this.uploaded.push(sequence);
    },
    async markDropped(_deviceId, sequence) {
      this.dropped.push(sequence);
    }
  };
}

describe("drainRelayQueue", () => {
  it("marks consumed uploads as uploaded and dropped uploads as dropped", async () => {
    const relayQueue = queue([message("1"), message("2")]);

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (relayMessage): Promise<RelayUploadOutcome> =>
        relayMessage.sequence === "1"
          ? { action: "consume", status: 202 }
          : { action: "drop", status: 401 },
      recordRetry: async () => {
        throw new Error("retry should not be recorded");
      }
    });

    expect(relayQueue.uploaded).toEqual(["1"]);
    expect(relayQueue.dropped).toEqual(["2"]);
  });

  it("records retry state without removing retryable messages", async () => {
    const relayQueue = queue([message("3")]);
    const retries: Array<{ sequence: string; delayMs: number }> = [];

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (): Promise<RelayUploadOutcome> => ({ action: "retry", status: 503 }),
      recordRetry: async (relayMessage, delayMs) => {
        retries.push({ sequence: relayMessage.sequence, delayMs });
      },
      random: () => 0.5
    });

    expect(relayQueue.uploaded).toEqual([]);
    expect(relayQueue.dropped).toEqual([]);
    expect(retries).toEqual([{ sequence: "3", delayMs: 1000 }]);
  });
});
