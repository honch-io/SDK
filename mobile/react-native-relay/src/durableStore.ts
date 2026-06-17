import type { StoredRelayMessage } from "./relayQueue";
import type { RelayRetryState } from "./relayQueue";

export type DurableRelayChunk = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  offset: number;
  frameBytes: Uint8Array;
  payload: Uint8Array;
  finalEnd?: number;
};

export interface RelayDurableStore {
  putChunk(chunk: DurableRelayChunk): Promise<void>;
  chunks(deviceId: string, sequence: string): Promise<DurableRelayChunk[]>;
  putCompleteMessage(message: StoredRelayMessage): Promise<void>;
  completeMessages(): Promise<StoredRelayMessage[]>;
  markRetry(deviceId: string, sequence: string, retry: RelayRetryState): Promise<void>;
  deleteMessage(deviceId: string, sequence: string): Promise<void>;
}

export type MemoryDurableStoreOptions = {
  // Upper bounds on retained complete messages and partial-reassembly chunks.
  // When exceeded, the oldest entries are evicted (drop-oldest), matching the
  // core SDK's bounded RAM-queue policy so the in-memory store cannot grow
  // without bound when uploads stall. Defaults match the MMKV store.
  maxCompleteMessages?: number;
  maxChunks?: number;
};

const DEFAULT_MAX_CHUNKS = 4096;
const DEFAULT_MAX_COMPLETE_MESSAGES = 1024;

export function createMemoryDurableStore(options?: MemoryDurableStoreOptions): RelayDurableStore {
  const maxMessages = options?.maxCompleteMessages ?? DEFAULT_MAX_COMPLETE_MESSAGES;
  const maxChunks = options?.maxChunks ?? DEFAULT_MAX_CHUNKS;
  const chunks: DurableRelayChunk[] = [];
  const messages: StoredRelayMessage[] = [];

  return {
    async putChunk(chunk) {
      const existing = chunks.find(
        (entry) =>
          entry.deviceId === chunk.deviceId &&
          entry.sequence === chunk.sequence &&
          entry.offset === chunk.offset
      );
      if (existing !== undefined) {
        Object.assign(existing, cloneChunk(chunk));
        return;
      }
      chunks.push(cloneChunk(chunk));
      while (chunks.length > maxChunks) {
        chunks.shift();
      }
    },

    async chunks(deviceId, sequence) {
      return chunks
        .filter((chunk) => chunk.deviceId === deviceId && chunk.sequence === sequence)
        .map(cloneChunk);
    },

    async putCompleteMessage(message) {
      const existing = messages.find(
        (entry) => entry.deviceId === message.deviceId && entry.sequence === message.sequence
      );
      if (existing !== undefined) {
        existing.body = new Uint8Array(message.body);
        existing.sourceType = message.sourceType;
        return;
      }
      messages.push(cloneMessage(message));
      while (messages.length > maxMessages) {
        messages.shift();
      }
    },

    async completeMessages() {
      return messages.map(cloneMessage);
    },

    async markRetry(deviceId, sequence, retry) {
      const message = messages.find(
        (entry) => entry.deviceId === deviceId && entry.sequence === sequence
      );
      if (message !== undefined) {
        message.retryAttempt = retry.attempt;
        message.nextAttemptAtMs = retry.nextAttemptAtMs;
      }
    },

    async deleteMessage(deviceId, sequence) {
      for (let i = messages.length - 1; i >= 0; i -= 1) {
        if (messages[i].deviceId === deviceId && messages[i].sequence === sequence) {
          messages.splice(i, 1);
        }
      }
      for (let i = chunks.length - 1; i >= 0; i -= 1) {
        if (chunks[i].deviceId === deviceId && chunks[i].sequence === sequence) {
          chunks.splice(i, 1);
        }
      }
    }
  };
}

function cloneChunk(chunk: DurableRelayChunk): DurableRelayChunk {
  return {
    ...chunk,
    frameBytes: new Uint8Array(chunk.frameBytes),
    payload: new Uint8Array(chunk.payload)
  };
}

function cloneMessage(message: StoredRelayMessage): StoredRelayMessage {
  return {
    ...message,
    body: new Uint8Array(message.body)
  };
}
