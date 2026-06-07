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
  retries: Array<{ sequence: string; attempt: number; nextAttemptAtMs: number }>;
} {
  return {
    uploaded: [],
    dropped: [],
    retries: [],
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
    },
    async markRetry(_deviceId, sequence, retry) {
      this.retries.push({ sequence, ...retry });
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
          : { action: "drop", status: 401 }
    });

    expect(relayQueue.uploaded).toEqual(["1"]);
    expect(relayQueue.dropped).toEqual(["2"]);
  });

  it("records retry state without removing retryable messages", async () => {
    const relayQueue = queue([message("3")]);

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (): Promise<RelayUploadOutcome> => ({ action: "retry", status: 503 }),
      now: () => 10000,
      random: () => 0.5
    });

    expect(relayQueue.uploaded).toEqual([]);
    expect(relayQueue.dropped).toEqual([]);
    expect(relayQueue.retries).toEqual([{ sequence: "3", attempt: 1, nextAttemptAtMs: 11000 }]);
  });

  it("uses Retry-After delay before local exponential backoff", async () => {
    const relayQueue = queue([message("4")]);

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (): Promise<RelayUploadOutcome> => ({
        action: "retry",
        status: 429,
        retryAfterMs: 12000
      }),
      now: () => 20000,
      random: () => 0.5
    });

    expect(relayQueue.retries).toEqual([{ sequence: "4", attempt: 1, nextAttemptAtMs: 32000 }]);
  });

  it("skips messages whose retry delay is not due yet", async () => {
    const relayQueue = queue([
      {
        ...message("5"),
        retryAttempt: 1,
        nextAttemptAtMs: 50000
      }
    ]);
    let uploads = 0;

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (): Promise<RelayUploadOutcome> => {
        uploads += 1;
        return { action: "consume", status: 202 };
      },
      now: () => 49000
    });

    expect(uploads).toBe(0);
    expect(relayQueue.uploaded).toEqual([]);
  });

  it("uses persisted retry attempts for exponential backoff", async () => {
    const relayQueue = queue([
      {
        ...message("6"),
        retryAttempt: 2,
        nextAttemptAtMs: 10000
      }
    ]);

    await drainRelayQueue({
      queue: relayQueue,
      upload: async (): Promise<RelayUploadOutcome> => ({ action: "retry", status: 503 }),
      now: () => 10000,
      random: () => 0.5
    });

    expect(relayQueue.retries).toEqual([{ sequence: "6", attempt: 3, nextAttemptAtMs: 14000 }]);
  });
});
