import type { MMKV } from "react-native-mmkv";

import type { DurableRelayChunk, RelayDurableStore } from "../durableStore";
import type { StoredRelayMessage } from "../relayQueue";

type RelayMmkvStorage = Pick<MMKV, "getString" | "set" | "remove">;

export type MmkvRelayStoreOptions = {
  maxChunks?: number;
  maxCompleteMessages?: number;
  ttlMs?: number;
  now?: () => number;
};

type SerializedRelayStoreIndex = {
  chunks: SerializedRelayChunkIndexEntry[];
  messages: SerializedRelayMessageIndexEntry[];
};

type SerializedRelayChunkIndexEntry = {
  key: string;
  deviceId: string;
  sourceType: number;
  sequence: string;
  offset: number;
  storedAtMs: number;
  finalEnd?: number;
};

type SerializedRelayMessageIndexEntry = {
  key: string;
  deviceId: string;
  sourceType: number;
  sequence: string;
  storedAtMs: number;
};

type SerializedRelayChunkRecord = {
  frameBytes: number[];
  payload: number[];
};

type SerializedRelayMessageRecord = {
  body: number[];
};

type LegacySerializedRelayStoreState = {
  chunks?: LegacySerializedRelayChunk[];
  messages?: LegacySerializedRelayMessage[];
};

type LegacySerializedRelayChunk = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  offset: number;
  frameBytes: number[];
  payload: number[];
  finalEnd?: number;
};

type LegacySerializedRelayMessage = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  body: number[];
};

const LEGACY_RELAY_STORE_KEY = "honch.relay.queue.v1";
const RELAY_INDEX_KEY = "honch.relay.index.v2";
const DEFAULT_MAX_CHUNKS = 4096;
const DEFAULT_MAX_COMPLETE_MESSAGES = 1024;
const DEFAULT_TTL_MS = 7 * 24 * 60 * 60 * 1000;

export function createMmkvRelayStore(
  storage: RelayMmkvStorage,
  options: MmkvRelayStoreOptions = {}
): RelayDurableStore {
  return new MmkvRelayStore(storage, {
    maxChunks: options.maxChunks ?? DEFAULT_MAX_CHUNKS,
    maxCompleteMessages: options.maxCompleteMessages ?? DEFAULT_MAX_COMPLETE_MESSAGES,
    ttlMs: options.ttlMs ?? DEFAULT_TTL_MS,
    now: options.now ?? Date.now
  });
}

class MmkvRelayStore implements RelayDurableStore {
  constructor(
    private readonly storage: RelayMmkvStorage,
    private readonly options: Required<MmkvRelayStoreOptions>
  ) {}

  async putChunk(chunk: DurableRelayChunk): Promise<void> {
    const index = this.prunedIndex();
    const key = chunkKey(chunk.deviceId, chunk.sequence, chunk.offset);
    const storedAtMs = this.options.now();
    const entry: SerializedRelayChunkIndexEntry = {
      key,
      deviceId: chunk.deviceId,
      sourceType: chunk.sourceType,
      sequence: chunk.sequence,
      offset: chunk.offset,
      storedAtMs,
      finalEnd: chunk.finalEnd
    };

    this.storage.set(
      key,
      JSON.stringify({
        frameBytes: Array.from(chunk.frameBytes),
        payload: Array.from(chunk.payload)
      } satisfies SerializedRelayChunkRecord)
    );

    const existing = index.chunks.findIndex(
      (candidate) =>
        candidate.deviceId === chunk.deviceId &&
        candidate.sequence === chunk.sequence &&
        candidate.offset === chunk.offset
    );
    if (existing >= 0) {
      index.chunks[existing] = entry;
    } else {
      index.chunks.push(entry);
    }

    this.enforceLimits(index);
    this.writeIndex(index);
  }

  async chunks(deviceId: string, sequence: string): Promise<DurableRelayChunk[]> {
    const index = this.prunedIndex();
    this.writeIndex(index);
    return index.chunks
      .filter((chunk) => chunk.deviceId === deviceId && chunk.sequence === sequence)
      .map((entry) => this.readChunk(entry))
      .filter((chunk): chunk is DurableRelayChunk => chunk !== undefined);
  }

  async putCompleteMessage(message: StoredRelayMessage): Promise<void> {
    const index = this.prunedIndex();
    const key = messageKey(message.deviceId, message.sequence);
    const storedAtMs = this.options.now();
    const entry: SerializedRelayMessageIndexEntry = {
      key,
      deviceId: message.deviceId,
      sourceType: message.sourceType,
      sequence: message.sequence,
      storedAtMs
    };

    this.storage.set(
      key,
      JSON.stringify({
        body: Array.from(message.body)
      } satisfies SerializedRelayMessageRecord)
    );

    const existing = index.messages.findIndex(
      (candidate) => candidate.deviceId === message.deviceId && candidate.sequence === message.sequence
    );
    if (existing >= 0) {
      index.messages[existing] = entry;
    } else {
      index.messages.push(entry);
    }

    this.enforceLimits(index);
    this.writeIndex(index);
  }

  async completeMessages(): Promise<StoredRelayMessage[]> {
    const index = this.prunedIndex();
    this.writeIndex(index);
    return index.messages
      .map((entry) => this.readMessage(entry))
      .filter((message): message is StoredRelayMessage => message !== undefined);
  }

  async deleteMessage(deviceId: string, sequence: string): Promise<void> {
    const index = this.prunedIndex();
    index.messages = index.messages.filter((message) => {
      if (message.deviceId !== deviceId || message.sequence !== sequence) {
        return true;
      }
      this.storage.remove(message.key);
      return false;
    });
    index.chunks = index.chunks.filter((chunk) => {
      if (chunk.deviceId !== deviceId || chunk.sequence !== sequence) {
        return true;
      }
      this.storage.remove(chunk.key);
      return false;
    });
    this.writeIndex(index);
  }

  private prunedIndex(): SerializedRelayStoreIndex {
    const index = this.readIndex();
    const expiresBeforeMs = this.options.now() - this.options.ttlMs;

    index.chunks = index.chunks.filter((chunk) => {
      if (chunk.storedAtMs >= expiresBeforeMs) {
        return true;
      }
      this.storage.remove(chunk.key);
      return false;
    });
    index.messages = index.messages.filter((message) => {
      if (message.storedAtMs >= expiresBeforeMs) {
        return true;
      }
      this.storage.remove(message.key);
      return false;
    });
    this.enforceLimits(index);
    return index;
  }

  private enforceLimits(index: SerializedRelayStoreIndex): void {
    index.chunks.sort((left, right) => left.storedAtMs - right.storedAtMs);
    while (index.chunks.length > this.options.maxChunks) {
      const [removed] = index.chunks.splice(0, 1);
      this.storage.remove(removed.key);
    }

    index.messages.sort((left, right) => left.storedAtMs - right.storedAtMs);
    while (index.messages.length > this.options.maxCompleteMessages) {
      const [removed] = index.messages.splice(0, 1);
      this.storage.remove(removed.key);
      index.chunks = index.chunks.filter((chunk) => {
        if (chunk.deviceId !== removed.deviceId || chunk.sequence !== removed.sequence) {
          return true;
        }
        this.storage.remove(chunk.key);
        return false;
      });
    }
  }

  private readIndex(): SerializedRelayStoreIndex {
    const raw = this.storage.getString(RELAY_INDEX_KEY);
    if (raw === undefined) {
      return this.migrateLegacyIndex();
    }
    const parsed = JSON.parse(raw) as SerializedRelayStoreIndex;
    return {
      chunks: parsed.chunks ?? [],
      messages: parsed.messages ?? []
    };
  }

  private writeIndex(index: SerializedRelayStoreIndex): void {
    if (index.chunks.length === 0 && index.messages.length === 0) {
      this.storage.remove(RELAY_INDEX_KEY);
      return;
    }
    this.storage.set(RELAY_INDEX_KEY, JSON.stringify(index));
  }

  private migrateLegacyIndex(): SerializedRelayStoreIndex {
    const raw = this.storage.getString(LEGACY_RELAY_STORE_KEY);
    if (raw === undefined) {
      return { chunks: [], messages: [] };
    }

    const legacy = JSON.parse(raw) as LegacySerializedRelayStoreState;
    const storedAtMs = this.options.now();
    const index: SerializedRelayStoreIndex = {
      chunks: [],
      messages: []
    };

    for (const chunk of legacy.chunks ?? []) {
      const key = chunkKey(chunk.deviceId, chunk.sequence, chunk.offset);
      this.storage.set(
        key,
        JSON.stringify({
          frameBytes: chunk.frameBytes,
          payload: chunk.payload
        } satisfies SerializedRelayChunkRecord)
      );
      index.chunks.push({
        key,
        deviceId: chunk.deviceId,
        sourceType: chunk.sourceType,
        sequence: chunk.sequence,
        offset: chunk.offset,
        storedAtMs,
        finalEnd: chunk.finalEnd
      });
    }

    for (const message of legacy.messages ?? []) {
      const key = messageKey(message.deviceId, message.sequence);
      this.storage.set(
        key,
        JSON.stringify({
          body: message.body
        } satisfies SerializedRelayMessageRecord)
      );
      index.messages.push({
        key,
        deviceId: message.deviceId,
        sourceType: message.sourceType,
        sequence: message.sequence,
        storedAtMs
      });
    }

    this.storage.remove(LEGACY_RELAY_STORE_KEY);
    return index;
  }

  private readChunk(entry: SerializedRelayChunkIndexEntry): DurableRelayChunk | undefined {
    const raw = this.storage.getString(entry.key);
    if (raw === undefined) {
      return undefined;
    }
    const record = JSON.parse(raw) as SerializedRelayChunkRecord;
    return {
      deviceId: entry.deviceId,
      sourceType: entry.sourceType,
      sequence: entry.sequence,
      offset: entry.offset,
      frameBytes: new Uint8Array(record.frameBytes),
      payload: new Uint8Array(record.payload),
      finalEnd: entry.finalEnd
    };
  }

  private readMessage(entry: SerializedRelayMessageIndexEntry): StoredRelayMessage | undefined {
    const raw = this.storage.getString(entry.key);
    if (raw === undefined) {
      return undefined;
    }
    const record = JSON.parse(raw) as SerializedRelayMessageRecord;
    return {
      deviceId: entry.deviceId,
      sourceType: entry.sourceType,
      sequence: entry.sequence,
      body: new Uint8Array(record.body)
    };
  }
}

function chunkKey(deviceId: string, sequence: string, offset: number): string {
  return `honch.relay.chunk.${deviceId}.${sequence}.${offset}`;
}

function messageKey(deviceId: string, sequence: string): string {
  return `honch.relay.message.${deviceId}.${sequence}`;
}
