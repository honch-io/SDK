import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

import type { DurableRelayChunk, RelayDurableStore } from "../durableStore";
import type { RelayRetryState } from "../relayQueue";
import type { StoredRelayMessage } from "../relayQueue";

type JsonRelayStoreState = {
  chunks: JsonRelayChunk[];
  messages: JsonRelayMessage[];
};

type JsonRelayChunk = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  offset: number;
  frameBytes: number[];
  payload: number[];
  finalEnd?: number;
};

type JsonRelayMessage = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  body: number[];
  retryAttempt?: number;
  nextAttemptAtMs?: number;
};

const EMPTY_STATE: JsonRelayStoreState = {
  chunks: [],
  messages: []
};

// Defaults match the MMKV store so retention is consistent across relay stores.
const DEFAULT_MAX_CHUNKS = 4096;
const DEFAULT_MAX_COMPLETE_MESSAGES = 1024;

export type JsonFileRelayStoreOptions = {
  // Bound the on-disk store so it cannot grow without limit when uploads stall;
  // when exceeded, the oldest entries are dropped (drop-oldest). No time TTL.
  maxChunks?: number;
  maxCompleteMessages?: number;
};

export function createJsonFileRelayStore(
  filePath: string,
  options: JsonFileRelayStoreOptions = {}
): RelayDurableStore {
  return new JsonFileRelayStore(
    filePath,
    options.maxChunks ?? DEFAULT_MAX_CHUNKS,
    options.maxCompleteMessages ?? DEFAULT_MAX_COMPLETE_MESSAGES
  );
}

class JsonFileRelayStore implements RelayDurableStore {
  constructor(
    private readonly filePath: string,
    private readonly maxChunks: number,
    private readonly maxCompleteMessages: number
  ) {}

  async putChunk(chunk: DurableRelayChunk): Promise<void> {
    const state = await this.readState();
    const serialized = serializeChunk(chunk);
    const index = state.chunks.findIndex(
      (entry) =>
        entry.deviceId === chunk.deviceId &&
        entry.sequence === chunk.sequence &&
        entry.offset === chunk.offset
    );
    if (index >= 0) {
      state.chunks[index] = serialized;
    } else {
      state.chunks.push(serialized);
      while (state.chunks.length > this.maxChunks) {
        state.chunks.shift();
      }
    }
    await this.writeState(state);
  }

  async chunks(deviceId: string, sequence: string): Promise<DurableRelayChunk[]> {
    const state = await this.readState();
    return state.chunks
      .filter((chunk) => chunk.deviceId === deviceId && chunk.sequence === sequence)
      .map(deserializeChunk);
  }

  async putCompleteMessage(message: StoredRelayMessage): Promise<void> {
    const state = await this.readState();
    const serialized = serializeMessage(message);
    const index = state.messages.findIndex(
      (entry) => entry.deviceId === message.deviceId && entry.sequence === message.sequence
    );
    if (index >= 0) {
      state.messages[index] = serialized;
    } else {
      state.messages.push(serialized);
      while (state.messages.length > this.maxCompleteMessages) {
        state.messages.shift();
      }
    }
    await this.writeState(state);
  }

  async completeMessages(): Promise<StoredRelayMessage[]> {
    const state = await this.readState();
    return state.messages.map(deserializeMessage);
  }

  async markRetry(deviceId: string, sequence: string, retry: RelayRetryState): Promise<void> {
    const state = await this.readState();
    const message = state.messages.find(
      (entry) => entry.deviceId === deviceId && entry.sequence === sequence
    );
    if (message !== undefined) {
      message.retryAttempt = retry.attempt;
      message.nextAttemptAtMs = retry.nextAttemptAtMs;
      await this.writeState(state);
    }
  }

  async deleteMessage(deviceId: string, sequence: string): Promise<void> {
    const state = await this.readState();
    state.messages = state.messages.filter(
      (message) => message.deviceId !== deviceId || message.sequence !== sequence
    );
    state.chunks = state.chunks.filter(
      (chunk) => chunk.deviceId !== deviceId || chunk.sequence !== sequence
    );
    await this.writeState(state);
  }

  private async readState(): Promise<JsonRelayStoreState> {
    try {
      const raw = await readFile(this.filePath, "utf8");
      const parsed = JSON.parse(raw) as JsonRelayStoreState;
      return {
        chunks: parsed.chunks ?? [],
        messages: parsed.messages ?? []
      };
    } catch (error) {
      if (error instanceof Error && "code" in error && error.code === "ENOENT") {
        return { chunks: [], messages: [] };
      }
      throw error;
    }
  }

  private async writeState(state: JsonRelayStoreState): Promise<void> {
    await mkdir(dirname(this.filePath), { recursive: true });
    const tempPath = `${this.filePath}.tmp`;
    await writeFile(tempPath, JSON.stringify(state));
    await rename(tempPath, this.filePath);
  }
}

function serializeChunk(chunk: DurableRelayChunk): JsonRelayChunk {
  return {
    deviceId: chunk.deviceId,
    sourceType: chunk.sourceType,
    sequence: chunk.sequence,
    offset: chunk.offset,
    frameBytes: Array.from(chunk.frameBytes),
    payload: Array.from(chunk.payload),
    finalEnd: chunk.finalEnd
  };
}

function deserializeChunk(chunk: JsonRelayChunk): DurableRelayChunk {
  return {
    deviceId: chunk.deviceId,
    sourceType: chunk.sourceType,
    sequence: chunk.sequence,
    offset: chunk.offset,
    frameBytes: new Uint8Array(chunk.frameBytes),
    payload: new Uint8Array(chunk.payload),
    finalEnd: chunk.finalEnd
  };
}

function serializeMessage(message: StoredRelayMessage): JsonRelayMessage {
  return {
    deviceId: message.deviceId,
    sourceType: message.sourceType,
    sequence: message.sequence,
    body: Array.from(message.body),
    retryAttempt: message.retryAttempt,
    nextAttemptAtMs: message.nextAttemptAtMs
  };
}

function deserializeMessage(message: JsonRelayMessage): StoredRelayMessage {
  return {
    deviceId: message.deviceId,
    sourceType: message.sourceType,
    sequence: message.sequence,
    body: new Uint8Array(message.body),
    retryAttempt: message.retryAttempt,
    nextAttemptAtMs: message.nextAttemptAtMs
  };
}
