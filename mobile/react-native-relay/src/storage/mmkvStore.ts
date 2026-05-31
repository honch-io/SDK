import type { MMKV } from "react-native-mmkv";

import type { DurableRelayChunk, RelayDurableStore } from "../durableStore";
import type { StoredRelayMessage } from "../relayQueue";

type RelayMmkvStorage = Pick<MMKV, "getString" | "set" | "remove">;

type SerializedRelayStoreState = {
  chunks: SerializedRelayChunk[];
  messages: SerializedRelayMessage[];
};

type SerializedRelayChunk = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  offset: number;
  frameBytes: number[];
  payload: number[];
  finalEnd?: number;
};

type SerializedRelayMessage = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  body: number[];
};

const RELAY_STORE_KEY = "honch.relay.queue.v1";

export function createMmkvRelayStore(storage: RelayMmkvStorage): RelayDurableStore {
  return new MmkvRelayStore(storage);
}

class MmkvRelayStore implements RelayDurableStore {
  constructor(private readonly storage: RelayMmkvStorage) {}

  async putChunk(chunk: DurableRelayChunk): Promise<void> {
    const state = this.readState();
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
    }
    this.writeState(state);
  }

  async chunks(deviceId: string, sequence: string): Promise<DurableRelayChunk[]> {
    return this.readState()
      .chunks.filter((chunk) => chunk.deviceId === deviceId && chunk.sequence === sequence)
      .map(deserializeChunk);
  }

  async putCompleteMessage(message: StoredRelayMessage): Promise<void> {
    const state = this.readState();
    const serialized = serializeMessage(message);
    const index = state.messages.findIndex(
      (entry) => entry.deviceId === message.deviceId && entry.sequence === message.sequence
    );
    if (index >= 0) {
      state.messages[index] = serialized;
    } else {
      state.messages.push(serialized);
    }
    this.writeState(state);
  }

  async completeMessages(): Promise<StoredRelayMessage[]> {
    return this.readState().messages.map(deserializeMessage);
  }

  async deleteMessage(deviceId: string, sequence: string): Promise<void> {
    const state = this.readState();
    state.messages = state.messages.filter(
      (message) => message.deviceId !== deviceId || message.sequence !== sequence
    );
    state.chunks = state.chunks.filter(
      (chunk) => chunk.deviceId !== deviceId || chunk.sequence !== sequence
    );
    this.writeState(state);
  }

  private readState(): SerializedRelayStoreState {
    const raw = this.storage.getString(RELAY_STORE_KEY);
    if (raw === undefined) {
      return { chunks: [], messages: [] };
    }
    const parsed = JSON.parse(raw) as SerializedRelayStoreState;
    return {
      chunks: parsed.chunks ?? [],
      messages: parsed.messages ?? []
    };
  }

  private writeState(state: SerializedRelayStoreState): void {
    if (state.chunks.length === 0 && state.messages.length === 0) {
      this.storage.remove(RELAY_STORE_KEY);
      return;
    }
    this.storage.set(RELAY_STORE_KEY, JSON.stringify(state));
  }
}

function serializeChunk(chunk: DurableRelayChunk): SerializedRelayChunk {
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

function deserializeChunk(chunk: SerializedRelayChunk): DurableRelayChunk {
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

function serializeMessage(message: StoredRelayMessage): SerializedRelayMessage {
  return {
    deviceId: message.deviceId,
    sourceType: message.sourceType,
    sequence: message.sequence,
    body: Array.from(message.body)
  };
}

function deserializeMessage(message: SerializedRelayMessage): StoredRelayMessage {
  return {
    deviceId: message.deviceId,
    sourceType: message.sourceType,
    sequence: message.sequence,
    body: new Uint8Array(message.body)
  };
}
