import Foundation

public actor MemoryRelayStore: RelayDurableStore {
    private var chunkRecords: [String: [DurableRelayChunk]] = [:]
    private var messages: [String: StoredRelayMessage] = [:]

    public init() {}

    public func putChunk(_ chunk: DurableRelayChunk) async throws {
        let key = Self.key(deviceId: chunk.deviceId, sequence: chunk.sequence)
        var existing = chunkRecords[key, default: []]
        existing.append(chunk)
        chunkRecords[key] = existing
    }

    public func chunks(deviceId: String, sequence: String) async throws -> [DurableRelayChunk] {
        chunkRecords[Self.key(deviceId: deviceId, sequence: sequence), default: []]
    }

    public func putCompleteMessage(_ message: StoredRelayMessage) async throws {
        messages[Self.key(deviceId: message.deviceId, sequence: message.sequence)] = message
    }

    public func completeMessages() async throws -> [StoredRelayMessage] {
        messages.values.sorted {
            if $0.deviceId == $1.deviceId {
                return $0.sequence < $1.sequence
            }
            return $0.deviceId < $1.deviceId
        }
    }

    public func deleteMessage(deviceId: String, sequence: String) async throws {
        let key = Self.key(deviceId: deviceId, sequence: sequence)
        messages.removeValue(forKey: key)
        chunkRecords.removeValue(forKey: key)
    }

    public func markRetry(deviceId: String, sequence: String, retry: RelayRetryState) async throws {
        let key = Self.key(deviceId: deviceId, sequence: sequence)
        guard var message = messages[key] else {
            return
        }
        message.retryAttempt = retry.attempt
        message.nextAttemptAtMs = retry.nextAttemptAtMs
        messages[key] = message
    }

    private static func key(deviceId: String, sequence: String) -> String {
        "\(deviceId)\u{0}\(sequence)"
    }
}
