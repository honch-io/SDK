import type { RelayQueue, StoredRelayMessage } from "./relayQueue";

export type RelayFrameAcknowledgement = {
  deviceId: string;
  sequence: string;
  ackBytes: Uint8Array;
  message: StoredRelayMessage;
};

export type RelayFrameAcknowledger = (
  acknowledgement: RelayFrameAcknowledgement
) => Promise<void> | void;

export type RelayFrameReceipt = {
  complete: boolean;
  message?: StoredRelayMessage;
  ackBytes?: Uint8Array;
};

export type RelayFrameReceiverOptions = {
  queue: RelayQueue;
};

export type RelayReceiveFrameOptions = {
  acknowledge?: RelayFrameAcknowledger;
};

export function buildRelayAck(sequence: string | number | bigint): Uint8Array {
  let value = BigInt(sequence);
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new Error("relay ACK sequence out of uint64 range");
  }

  const ackBytes = new Uint8Array(9);
  ackBytes[0] = 1;
  for (let index = 8; index >= 1; index -= 1) {
    ackBytes[index] = Number(value & 0xffn);
    value >>= 8n;
  }
  return ackBytes;
}

export function createRelayFrameReceiver(options: RelayFrameReceiverOptions) {
  return {
    async receiveFrame(
      deviceId: string,
      frameBytes: Uint8Array,
      receiveOptions: RelayReceiveFrameOptions = {}
    ): Promise<RelayFrameReceipt> {
      const result = await options.queue.putChunk(deviceId, frameBytes);
      if (!result.complete || result.message === undefined) {
        return { complete: false };
      }

      const ackBytes = buildRelayAck(result.message.sequence);
      if (receiveOptions.acknowledge !== undefined) {
        await receiveOptions.acknowledge({
          deviceId,
          sequence: result.message.sequence,
          ackBytes,
          message: result.message
        });
      }
      return { complete: true, message: result.message, ackBytes };
    }
  };
}
