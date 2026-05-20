export type StoredRelayMessage = {
  id: string;
  frame: Uint8Array;
  receivedAt: number;
};

export type RelayQueue = {
  enqueue(message: StoredRelayMessage): Promise<void>;
  peek(limit: number): Promise<StoredRelayMessage[]>;
  remove(ids: string[]): Promise<void>;
};
