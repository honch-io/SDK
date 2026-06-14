import Foundation

public struct RelayChunkReceipt: Equatable, Sendable {
    public let complete: Bool
    public let message: StoredRelayMessage?
}

public actor RelayQueue {
    private let store: any RelayDurableStore

    public init(store: any RelayDurableStore) {
        self.store = store
    }

    public func putChunk(deviceId: String, frameBytes: Data) async throws -> RelayChunkReceipt {
        let frame = try RelayFrameDecoder.decode(frameBytes)
        let sequence = String(frame.sequence)
        let existingChunks = try await store.chunks(deviceId: deviceId, sequence: sequence)

        if let existing = existingChunks.first(where: { $0.offset == frame.offset }) {
            guard existing.frameBytes == frameBytes else {
                throw RelayError.duplicateChunkMismatch
            }
            let existingMessage = try await store.completeMessages().first {
                $0.deviceId == deviceId && $0.sequence == sequence
            }
            return RelayChunkReceipt(
                complete: existingMessage != nil,
                message: existingMessage
            )
        }

        if existingChunks.contains(where: { $0.sourceType != frame.sourceType }) {
            throw RelayError.sourceTypeMismatch
        }

        let finalEnd: UInt32?
        if frame.final {
            let incomingFinalEnd = frame.offset + UInt32(frame.payload.count)
            if let existingFinalEnd = existingChunks.compactMap(\.finalEnd).first,
               existingFinalEnd != incomingFinalEnd {
                throw RelayError.finalChunkLengthMismatch
            }
            finalEnd = incomingFinalEnd
        } else {
            finalEnd = nil
        }

        let chunk = DurableRelayChunk(
            deviceId: deviceId,
            sourceType: frame.sourceType,
            sequence: sequence,
            offset: frame.offset,
            frameBytes: frameBytes,
            payload: frame.payload,
            finalEnd: finalEnd
        )
        try await store.putChunk(chunk)

        guard let message = completeMessageFromChunks(
            deviceId: deviceId,
            sourceType: frame.sourceType,
            sequence: sequence,
            chunks: existingChunks + [chunk]
        ) else {
            return RelayChunkReceipt(complete: false, message: nil)
        }

        try await store.putCompleteMessage(message)
        return RelayChunkReceipt(complete: true, message: message)
    }

    public func markUploaded(deviceId: String, sequence: String) async throws {
        try await store.deleteMessage(deviceId: deviceId, sequence: sequence)
    }

    public func markDropped(deviceId: String, sequence: String) async throws {
        try await store.deleteMessage(deviceId: deviceId, sequence: sequence)
    }

    public func markRetry(deviceId: String, sequence: String, retry: RelayRetryState) async throws {
        try await store.markRetry(deviceId: deviceId, sequence: sequence, retry: retry)
    }

    public func pending() async throws -> [StoredRelayMessage] {
        try await store.completeMessages()
    }
}

private func completeMessageFromChunks(
    deviceId: String,
    sourceType: UInt8,
    sequence: String,
    chunks: [DurableRelayChunk]
) -> StoredRelayMessage? {
    guard let finalEnd = chunks.compactMap(\.finalEnd).first else {
        return nil
    }

    let sorted = chunks.sorted { $0.offset < $1.offset }
    var cursor: UInt32 = 0
    var body = Data()
    for chunk in sorted {
        guard chunk.offset == cursor else {
            return nil
        }
        body.append(chunk.payload)
        cursor += UInt32(chunk.payload.count)
    }
    guard cursor == finalEnd else {
        return nil
    }

    return StoredRelayMessage(
        deviceId: deviceId,
        sourceType: sourceType,
        sequence: sequence,
        body: body
    )
}
