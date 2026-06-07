import type { MMKV } from "react-native-mmkv";

import type { DurableRelayChunk, RelayDurableStore } from "../durableStore";
import type { RelayRetryState } from "../relayQueue";
import type { StoredRelayMessage } from "../relayQueue";

type RelayMmkvStorage = Pick<MMKV, "getString" | "set" | "remove">;

export type MmkvRelayStoreOptions = {
  maxChunks?: number;
  maxCompleteMessages?: number;
  ttlMs?: number;
  keyPrefix?: string;
  now?: () => number;
};

type SerializedRelayStoreIndex = {
  chunks?: SerializedRelayChunkIndexEntry[];
  messages: SerializedRelayMessageIndexEntry[];
};

type SerializedRelaySequenceChunkIndex = {
  chunks: SerializedRelayChunkIndexEntry[];
};

type SerializedRelayChunkOrderState = {
  nextOrdinal: number;
  oldestOrdinal: number;
  count: number;
};

type SerializedRelayChunkOrderEntry = {
  key: string;
  deviceId: string;
  sequence: string;
  offset: number;
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
  retryAttempt?: number;
  nextAttemptAtMs?: number;
};

type SerializedRelayChunkRecord = {
  frameBase64?: string;
  payloadBase64?: string;
  frameBytes?: number[];
  payload?: number[];
};

type SerializedRelayMessageRecord = {
  bodyBase64?: string;
  body?: number[];
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
const DEFAULT_KEY_PREFIX = "honch.relay";
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
    keyPrefix: normalizeKeyPrefix(options.keyPrefix),
    now: options.now ?? Date.now
  });
}

class MmkvRelayStore implements RelayDurableStore {
  constructor(
    private readonly storage: RelayMmkvStorage,
    private readonly options: Required<MmkvRelayStoreOptions>
  ) {}

  async putChunk(chunk: DurableRelayChunk): Promise<void> {
    const sequenceIndex = this.prunedSequenceChunkIndex(chunk.deviceId, chunk.sequence);
    const key = this.chunkKey(chunk.deviceId, chunk.sequence, chunk.offset);
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
        frameBase64: bytesToBase64(chunk.frameBytes),
        payloadBase64: bytesToBase64(chunk.payload)
      } satisfies SerializedRelayChunkRecord)
    );

    const existing = sequenceIndex.chunks.findIndex(
      (candidate) =>
        candidate.deviceId === chunk.deviceId &&
        candidate.sequence === chunk.sequence &&
        candidate.offset === chunk.offset
    );
    if (existing >= 0) {
      sequenceIndex.chunks[existing] = entry;
    } else {
      sequenceIndex.chunks.push(entry);
      this.appendChunkOrderEntry(entry);
    }

    this.writeSequenceChunkIndex(chunk.deviceId, chunk.sequence, sequenceIndex.chunks);
    this.enforceChunkLimit();
  }

  async chunks(deviceId: string, sequence: string): Promise<DurableRelayChunk[]> {
    const sequenceIndex = this.prunedSequenceChunkIndex(deviceId, sequence);
    return sequenceIndex.chunks
      .map((entry) => this.readChunk(entry))
      .filter((chunk): chunk is DurableRelayChunk => chunk !== undefined);
  }

  async putCompleteMessage(message: StoredRelayMessage): Promise<void> {
    const index = this.prunedIndex();
    const key = this.messageKey(message.deviceId, message.sequence);
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
        bodyBase64: bytesToBase64(message.body)
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

  async markRetry(deviceId: string, sequence: string, retry: RelayRetryState): Promise<void> {
    const index = this.prunedIndex();
    const entry = index.messages.find(
      (message) => message.deviceId === deviceId && message.sequence === sequence
    );
    if (entry !== undefined) {
      entry.retryAttempt = retry.attempt;
      entry.nextAttemptAtMs = retry.nextAttemptAtMs;
      this.writeIndex(index);
    }
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
    for (const chunk of this.readSequenceChunkIndex(deviceId, sequence).chunks) {
      this.storage.remove(chunk.key);
    }
    this.storage.remove(this.sequenceChunkIndexKey(deviceId, sequence));
    this.writeIndex(index);
  }

  private prunedIndex(): SerializedRelayStoreIndex {
    const index = this.readIndex();
    const expiresBeforeMs = this.options.now() - this.options.ttlMs;

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
    index.messages.sort((left, right) => left.storedAtMs - right.storedAtMs);
    while (index.messages.length > this.options.maxCompleteMessages) {
      const [removed] = index.messages.splice(0, 1);
      this.storage.remove(removed.key);
      const sequenceIndex = this.readSequenceChunkIndex(removed.deviceId, removed.sequence);
      for (const chunk of sequenceIndex.chunks) {
        this.storage.remove(chunk.key);
      }
      this.storage.remove(this.sequenceChunkIndexKey(removed.deviceId, removed.sequence));
    }
  }

  private readIndex(): SerializedRelayStoreIndex {
    const raw = this.storage.getString(this.indexKey());
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
    if ((index.chunks?.length ?? 0) === 0 && index.messages.length === 0) {
      this.storage.remove(this.indexKey());
      return;
    }
    this.storage.set(this.indexKey(), JSON.stringify(index));
  }

  private readSequenceChunkIndex(
    deviceId: string,
    sequence: string
  ): SerializedRelaySequenceChunkIndex {
    const raw = this.storage.getString(this.sequenceChunkIndexKey(deviceId, sequence));
    if (raw !== undefined) {
      const parsed = JSON.parse(raw) as SerializedRelaySequenceChunkIndex;
      return { chunks: parsed.chunks ?? [] };
    }

    return this.readLegacySequenceChunkIndex(deviceId, sequence);
  }

  private prunedSequenceChunkIndex(deviceId: string, sequence: string): SerializedRelaySequenceChunkIndex {
    const sequenceIndex = this.readSequenceChunkIndex(deviceId, sequence);
    const expiresBeforeMs = this.options.now() - this.options.ttlMs;
    const chunks = sequenceIndex.chunks.filter((chunk) => {
      if (chunk.storedAtMs >= expiresBeforeMs) {
        return true;
      }
      this.storage.remove(chunk.key);
      return false;
    });
    this.writeSequenceChunkIndex(deviceId, sequence, chunks);
    return { chunks };
  }

  private writeSequenceChunkIndex(
    deviceId: string,
    sequence: string,
    chunks: SerializedRelayChunkIndexEntry[]
  ): void {
    const key = this.sequenceChunkIndexKey(deviceId, sequence);
    if (chunks.length === 0) {
      this.storage.remove(key);
      return;
    }
    this.storage.set(key, JSON.stringify({ chunks } satisfies SerializedRelaySequenceChunkIndex));
  }

  private appendChunkOrderEntry(entry: SerializedRelayChunkIndexEntry): void {
    const state = this.readChunkOrderState();
    const ordinal = state.nextOrdinal;
    this.storage.set(
      this.chunkOrderEntryKey(ordinal),
      JSON.stringify({
        key: entry.key,
        deviceId: entry.deviceId,
        sequence: entry.sequence,
        offset: entry.offset
      } satisfies SerializedRelayChunkOrderEntry)
    );
    this.writeChunkOrderState({
      nextOrdinal: ordinal + 1,
      oldestOrdinal: state.count === 0 ? ordinal : state.oldestOrdinal,
      count: state.count + 1
    });
  }

  private enforceChunkLimit(): void {
    let state = this.readChunkOrderState();
    while (state.count > this.options.maxChunks) {
      const orderKey = this.chunkOrderEntryKey(state.oldestOrdinal);
      const raw = this.storage.getString(orderKey);
      this.storage.remove(orderKey);
      if (raw !== undefined) {
        const entry = JSON.parse(raw) as SerializedRelayChunkOrderEntry;
        this.storage.remove(entry.key);
        const sequenceIndex = this.readSequenceChunkIndex(entry.deviceId, entry.sequence);
        this.writeSequenceChunkIndex(
          entry.deviceId,
          entry.sequence,
          sequenceIndex.chunks.filter((chunk) => chunk.offset !== entry.offset)
        );
      }
      state = {
        nextOrdinal: state.nextOrdinal,
        oldestOrdinal: state.oldestOrdinal + 1,
        count: state.count - 1
      };
    }
    this.writeChunkOrderState(state);
  }

  private readChunkOrderState(): SerializedRelayChunkOrderState {
    const raw = this.storage.getString(this.chunkOrderStateKey());
    if (raw === undefined) {
      return { nextOrdinal: 1, oldestOrdinal: 1, count: 0 };
    }
    const parsed = JSON.parse(raw) as Partial<SerializedRelayChunkOrderState>;
    return {
      nextOrdinal: parsed.nextOrdinal ?? 1,
      oldestOrdinal: parsed.oldestOrdinal ?? 1,
      count: parsed.count ?? 0
    };
  }

  private writeChunkOrderState(state: SerializedRelayChunkOrderState): void {
    if (state.count <= 0) {
      this.storage.remove(this.chunkOrderStateKey());
      return;
    }
    this.storage.set(this.chunkOrderStateKey(), JSON.stringify(state));
  }

  private readLegacySequenceChunkIndex(
    deviceId: string,
    sequence: string
  ): SerializedRelaySequenceChunkIndex {
    const raw = this.storage.getString(LEGACY_RELAY_STORE_KEY);
    if (raw === undefined) {
      return { chunks: [] };
    }

    const legacy = JSON.parse(raw) as LegacySerializedRelayStoreState;
    const storedAtMs = this.options.now();
    const chunksByOffset = new Map<number, SerializedRelayChunkIndexEntry>();
    for (const chunk of legacy.chunks ?? []) {
      if (chunk.deviceId !== deviceId || chunk.sequence !== sequence) {
        continue;
      }
      const key = this.chunkKey(chunk.deviceId, chunk.sequence, chunk.offset);
      this.storage.set(
        key,
        JSON.stringify({
          frameBase64: bytesToBase64(Uint8Array.from(chunk.frameBytes)),
          payloadBase64: bytesToBase64(Uint8Array.from(chunk.payload))
        } satisfies SerializedRelayChunkRecord)
      );
      chunksByOffset.set(chunk.offset, {
        key,
        deviceId: chunk.deviceId,
        sourceType: chunk.sourceType,
        sequence: chunk.sequence,
        offset: chunk.offset,
        storedAtMs,
        finalEnd: chunk.finalEnd
      });
    }
    const chunks = [...chunksByOffset.values()].sort((left, right) => left.offset - right.offset);
    this.writeSequenceChunkIndex(deviceId, sequence, chunks);
    return { chunks };
  }

  private migrateLegacyIndex(): SerializedRelayStoreIndex {
    const raw = this.storage.getString(LEGACY_RELAY_STORE_KEY);
    if (raw === undefined) {
      return { chunks: [], messages: [] };
    }

    const legacy = JSON.parse(raw) as LegacySerializedRelayStoreState;
    const storedAtMs = this.options.now();
    const index: SerializedRelayStoreIndex = {
      messages: []
    };
    const chunkIndexesBySequence = new Map<string, SerializedRelayChunkIndexEntry[]>();

    for (const chunk of legacy.chunks ?? []) {
      const key = this.chunkKey(chunk.deviceId, chunk.sequence, chunk.offset);
      this.storage.set(
        key,
        JSON.stringify({
          frameBase64: bytesToBase64(Uint8Array.from(chunk.frameBytes)),
          payloadBase64: bytesToBase64(Uint8Array.from(chunk.payload))
        } satisfies SerializedRelayChunkRecord)
      );
      const entry: SerializedRelayChunkIndexEntry = {
        key,
        deviceId: chunk.deviceId,
        sourceType: chunk.sourceType,
        sequence: chunk.sequence,
        offset: chunk.offset,
        storedAtMs,
        finalEnd: chunk.finalEnd
      };
      const sequenceKey = `${chunk.deviceId}\0${chunk.sequence}`;
      const sequenceIndex = chunkIndexesBySequence.get(sequenceKey) ?? [];
      const existing = sequenceIndex.findIndex((candidate) => candidate.offset === entry.offset);
      if (existing >= 0) {
        sequenceIndex[existing] = entry;
      } else {
        sequenceIndex.push(entry);
        this.appendChunkOrderEntry(entry);
      }
      chunkIndexesBySequence.set(sequenceKey, sequenceIndex);
    }

    for (const chunks of chunkIndexesBySequence.values()) {
      const [firstChunk] = chunks;
      if (firstChunk !== undefined) {
        this.writeSequenceChunkIndex(
          firstChunk.deviceId,
          firstChunk.sequence,
          chunks.sort((left, right) => left.offset - right.offset)
        );
      }
    }

    for (const message of legacy.messages ?? []) {
      const key = this.messageKey(message.deviceId, message.sequence);
      this.storage.set(
        key,
        JSON.stringify({
          bodyBase64: bytesToBase64(Uint8Array.from(message.body))
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
    const frameBytes =
      record.frameBase64 !== undefined ? base64ToBytes(record.frameBase64) : Uint8Array.from(record.frameBytes ?? []);
    const payload =
      record.payloadBase64 !== undefined ? base64ToBytes(record.payloadBase64) : Uint8Array.from(record.payload ?? []);
    return {
      deviceId: entry.deviceId,
      sourceType: entry.sourceType,
      sequence: entry.sequence,
      offset: entry.offset,
      frameBytes,
      payload,
      finalEnd: entry.finalEnd
    };
  }

  private readMessage(entry: SerializedRelayMessageIndexEntry): StoredRelayMessage | undefined {
    const raw = this.storage.getString(entry.key);
    if (raw === undefined) {
      return undefined;
    }
    const record = JSON.parse(raw) as SerializedRelayMessageRecord;
    const body = record.bodyBase64 !== undefined ? base64ToBytes(record.bodyBase64) : Uint8Array.from(record.body ?? []);
    return {
      deviceId: entry.deviceId,
      sourceType: entry.sourceType,
      sequence: entry.sequence,
      body,
      retryAttempt: entry.retryAttempt,
      nextAttemptAtMs: entry.nextAttemptAtMs
    };
  }

  private indexKey(): string {
    return `${this.options.keyPrefix}.index.v2`;
  }

  private chunkOrderStateKey(): string {
    return `${this.options.keyPrefix}.chunkOrder.state.v1`;
  }

  private chunkOrderEntryKey(ordinal: number): string {
    return `${this.options.keyPrefix}.chunkOrder.${ordinal}`;
  }

  private chunkKey(deviceId: string, sequence: string, offset: number): string {
    return `${this.options.keyPrefix}.chunk.${deviceId}.${sequence}.${offset}`;
  }

  private sequenceChunkIndexKey(deviceId: string, sequence: string): string {
    return `${this.options.keyPrefix}.chunks.${deviceId}.${sequence}.index.v1`;
  }

  private messageKey(deviceId: string, sequence: string): string {
    return `${this.options.keyPrefix}.message.${deviceId}.${sequence}`;
  }
}

function normalizeKeyPrefix(keyPrefix: string | undefined): string {
  const trimmed = keyPrefix?.trim().replace(/\.+$/g, "");
  return trimmed === undefined || trimmed.length === 0 ? DEFAULT_KEY_PREFIX : trimmed;
}

const BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

function bytesToBase64(bytes: Uint8Array): string {
  let output = "";
  for (let offset = 0; offset < bytes.length; offset += 3) {
    const first = bytes[offset] ?? 0;
    const second = bytes[offset + 1] ?? 0;
    const third = bytes[offset + 2] ?? 0;
    const packed = (first << 16) | (second << 8) | third;
    output += BASE64_ALPHABET[(packed >>> 18) & 0x3f];
    output += BASE64_ALPHABET[(packed >>> 12) & 0x3f];
    output += offset + 1 < bytes.length ? BASE64_ALPHABET[(packed >>> 6) & 0x3f] : "=";
    output += offset + 2 < bytes.length ? BASE64_ALPHABET[packed & 0x3f] : "=";
  }
  return output;
}

function base64ToBytes(base64: string): Uint8Array {
  const sanitized = base64.replace(/\s/g, "");
  const bytes: number[] = [];
  for (let offset = 0; offset < sanitized.length; offset += 4) {
    const chunk = sanitized.slice(offset, offset + 4);
    const first = decodeBase64Character(chunk[0]);
    const second = decodeBase64Character(chunk[1]);
    const third = decodeBase64Character(chunk[2]);
    const fourth = decodeBase64Character(chunk[3]);
    if (first === undefined || second === undefined) {
      break;
    }

    bytes.push((first << 2) | (second >> 4));
    if (chunk[2] !== "=" && third !== undefined) {
      bytes.push(((second & 0x0f) << 4) | (third >> 2));
    }
    if (chunk[3] !== "=" && third !== undefined && fourth !== undefined) {
      bytes.push(((third & 0x03) << 6) | fourth);
    }
  }
  return Uint8Array.from(bytes);
}

function decodeBase64Character(character: string | undefined): number | undefined {
  if (character === undefined) {
    return undefined;
  }
  if (character === "=") {
    return 0;
  }
  const value = BASE64_ALPHABET.indexOf(character);
  return value >= 0 ? value : undefined;
}
