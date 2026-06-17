import Foundation

public enum RelayError: Error, Equatable, Sendable {
    case notImplemented
    case frameTooShort
    case unsupportedFrameVersion(UInt8)
    case unsupportedSourceType(UInt8)
    case reservedByteMustBeZero
    case unknownFlagBits(UInt8)
    case payloadLengthMismatch
    case crcMismatch
    case duplicateChunkMismatch
    case sourceTypeMismatch
    case finalChunkLengthMismatch
    case ackSequenceOutOfRange
    case invalidWireV2MessageId
    case firstFlagOffsetMismatch
}

public struct DurableRelayChunk: Equatable, Sendable {
    public let deviceId: String
    public let sourceType: UInt8
    public let sequence: String
    public let offset: UInt32
    public let frameBytes: Data
    public let payload: Data
    public let finalEnd: UInt32?

    public init(
        deviceId: String,
        sourceType: UInt8,
        sequence: String,
        offset: UInt32,
        frameBytes: Data,
        payload: Data,
        finalEnd: UInt32? = nil
    ) {
        self.deviceId = deviceId
        self.sourceType = sourceType
        self.sequence = sequence
        self.offset = offset
        self.frameBytes = frameBytes
        self.payload = payload
        self.finalEnd = finalEnd
    }
}

public struct RelayRetryState: Equatable, Sendable {
    public let attempt: Int
    public let nextAttemptAtMs: Int64

    public init(attempt: Int, nextAttemptAtMs: Int64) {
        self.attempt = attempt
        self.nextAttemptAtMs = nextAttemptAtMs
    }
}

public protocol RelayDurableStore: Sendable {
    func putChunk(_ chunk: DurableRelayChunk) async throws
    func chunks(deviceId: String, sequence: String) async throws -> [DurableRelayChunk]
    func putCompleteMessage(_ message: StoredRelayMessage) async throws
    func completeMessages() async throws -> [StoredRelayMessage]
    func deleteMessage(deviceId: String, sequence: String) async throws
    func markRetry(deviceId: String, sequence: String, retry: RelayRetryState) async throws
}
