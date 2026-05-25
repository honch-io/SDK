import { decodeRelayFrame } from "./frame";
import type { DurableRelayChunk, RelayDurableStore } from "./durableStore";

export type StoredRelayMessage = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  body: Uint8Array;
};

export interface RelayQueue {
  putChunk(
    deviceId: string,
    frameBytes: Uint8Array
  ): Promise<{ complete: boolean; message?: StoredRelayMessage }>;
  markUploaded(deviceId: string, sequence: string): Promise<void>;
  markDropped(deviceId: string, sequence: string): Promise<void>;
  pending(): Promise<StoredRelayMessage[]>;
}

type StoredChunk = {
  offset: number;
  bytes: Uint8Array;
  payload: Uint8Array;
};

type Assembly = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  chunks: StoredChunk[];
  finalEnd?: number;
  message?: StoredRelayMessage;
};

export function createInMemoryRelayQueue(): RelayQueue {
  const assemblies = new Map<string, Assembly>();
  const completeMessages: StoredRelayMessage[] = [];

  function key(deviceId: string, sequence: string): string {
    return `${deviceId}\0${sequence}`;
  }

  function findChunk(assembly: Assembly, offset: number): StoredChunk | undefined {
    return assembly.chunks.find((chunk) => chunk.offset === offset);
  }

  function completeMessage(assembly: Assembly): StoredRelayMessage | undefined {
    if (assembly.finalEnd === undefined) {
      return undefined;
    }

    const chunks = [...assembly.chunks].sort((left, right) => left.offset - right.offset);
    let cursor = 0;
    for (const chunk of chunks) {
      if (chunk.offset !== cursor) {
        return undefined;
      }
      cursor += chunk.payload.length;
    }
    if (cursor !== assembly.finalEnd) {
      return undefined;
    }

    const body = new Uint8Array(cursor);
    for (const chunk of chunks) {
      body.set(chunk.payload, chunk.offset);
    }
    return {
      deviceId: assembly.deviceId,
      sourceType: assembly.sourceType,
      sequence: assembly.sequence,
      body
    };
  }

  return {
    async putChunk(deviceId, frameBytes) {
      const frame = decodeRelayFrame(frameBytes);
      const sequence = frame.sequence.toString();
      const assemblyKey = key(deviceId, sequence);
      let assembly = assemblies.get(assemblyKey);
      if (assembly === undefined) {
        assembly = {
          deviceId,
          sourceType: frame.sourceType,
          sequence,
          chunks: []
        };
        assemblies.set(assemblyKey, assembly);
      } else if (assembly.sourceType !== frame.sourceType) {
        throw new Error("relay frame source type mismatch");
      }

      const existing = findChunk(assembly, frame.offset);
      if (existing !== undefined) {
        if (!bytesEqual(existing.bytes, frameBytes)) {
          throw new Error("relay duplicate chunk mismatch");
        }
        return { complete: assembly.message !== undefined, message: assembly.message };
      }

      assembly.chunks.push({
        offset: frame.offset,
        bytes: new Uint8Array(frameBytes),
        payload: new Uint8Array(frame.payload)
      });
      if (frame.final) {
        assembly.finalEnd = frame.offset + frame.payload.length;
      }

      const message = completeMessage(assembly);
      if (message === undefined) {
        return { complete: false };
      }

      assembly.message = message;
      if (!completeMessages.some((entry) => entry.deviceId === deviceId && entry.sequence === sequence)) {
        completeMessages.push(message);
      }
      return { complete: true, message };
    },

    async markUploaded(deviceId, sequence) {
      const assemblyKey = key(deviceId, sequence);
      assemblies.delete(assemblyKey);
      const index = completeMessages.findIndex(
        (message) => message.deviceId === deviceId && message.sequence === sequence
      );
      if (index >= 0) {
        completeMessages.splice(index, 1);
      }
    },

    async markDropped(deviceId, sequence) {
      await this.markUploaded(deviceId, sequence);
    },

    async pending() {
      return completeMessages.map((message) => ({
        ...message,
        body: new Uint8Array(message.body)
      }));
    }
  };
}

export function createDurableRelayQueue(store: RelayDurableStore): RelayQueue {
  return {
    async putChunk(deviceId, frameBytes) {
      const frame = decodeRelayFrame(frameBytes);
      const sequence = frame.sequence.toString();
      const existingChunks = await store.chunks(deviceId, sequence);
      const existing = existingChunks.find((chunk) => chunk.offset === frame.offset);
      if (existing !== undefined) {
        if (!bytesEqual(existing.frameBytes, frameBytes)) {
          throw new Error("relay duplicate chunk mismatch");
        }
        const existingMessage = (await store.completeMessages()).find(
          (message) => message.deviceId === deviceId && message.sequence === sequence
        );
        return { complete: existingMessage !== undefined, message: existingMessage };
      }

      if (
        existingChunks.length > 0 &&
        existingChunks.some((chunk) => chunk.sourceType !== frame.sourceType)
      ) {
        throw new Error("relay frame source type mismatch");
      }

      const chunk: DurableRelayChunk = {
        deviceId,
        sourceType: frame.sourceType,
        sequence,
        offset: frame.offset,
        frameBytes: new Uint8Array(frameBytes),
        payload: new Uint8Array(frame.payload)
      };
      if (frame.final) {
        chunk.finalEnd = frame.offset + frame.payload.length;
      }
      await store.putChunk(chunk);

      const message = completeMessageFromChunks(deviceId, frame.sourceType, sequence, [
        ...existingChunks,
        chunk
      ]);
      if (message === undefined) {
        return { complete: false };
      }

      await store.putCompleteMessage(message);
      return { complete: true, message };
    },

    async markUploaded(deviceId, sequence) {
      await store.deleteMessage(deviceId, sequence);
    },

    async markDropped(deviceId, sequence) {
      await store.deleteMessage(deviceId, sequence);
    },

    async pending() {
      return store.completeMessages();
    }
  };
}

function completeMessageFromChunks(
  deviceId: string,
  sourceType: number,
  sequence: string,
  chunks: DurableRelayChunk[]
): StoredRelayMessage | undefined {
  const finalEnd = chunks.find((chunk) => chunk.finalEnd !== undefined)?.finalEnd;
  if (finalEnd === undefined) {
    return undefined;
  }

  const sorted = [...chunks].sort((left, right) => left.offset - right.offset);
  let cursor = 0;
  for (const chunk of sorted) {
    if (chunk.offset !== cursor) {
      return undefined;
    }
    cursor += chunk.payload.length;
  }
  if (cursor !== finalEnd) {
    return undefined;
  }

  const body = new Uint8Array(cursor);
  for (const chunk of sorted) {
    body.set(chunk.payload, chunk.offset);
  }
  return { deviceId, sourceType, sequence, body };
}

function bytesEqual(left: Uint8Array, right: Uint8Array): boolean {
  if (left.length !== right.length) {
    return false;
  }
  for (let i = 0; i < left.length; i += 1) {
    if (left[i] !== right[i]) {
      return false;
    }
  }
  return true;
}
