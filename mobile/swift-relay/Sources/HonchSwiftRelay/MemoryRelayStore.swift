import Foundation

public actor MemoryRelayStore: RelayDurableStore {
    private var chunkRecords: [String: [DurableRelayChunk]] = [:]
    private var messages: [String: StoredRelayMessage] = [:]
    // Insertion-order keys, oldest first, so the store can drop-oldest when it
    // hits its caps -- matching the core SDK's bounded queue policy so it cannot
    // grow without bound when uploads stall. No time TTL.
    private var chunkGroupOrder: [String] = []
    private var messageOrder: [String] = []
    private let maxChunkGroups: Int
    private let maxCompleteMessages: Int

    public init(maxChunkGroups: Int = 4096, maxCompleteMessages: Int = 1024) {
        self.maxChunkGroups = maxChunkGroups
        self.maxCompleteMessages = maxCompleteMessages
    }

    public func putChunk(_ chunk: DurableRelayChunk) async throws {
        let key = Self.key(deviceId: chunk.deviceId, sequence: chunk.sequence)
        if chunkRecords[key] == nil {
            chunkGroupOrder.append(key)
        }
        var existing = chunkRecords[key, default: []]
        existing.append(chunk)
        chunkRecords[key] = existing
        while chunkRecords.count > maxChunkGroups, !chunkGroupOrder.isEmpty {
            chunkRecords.removeValue(forKey: chunkGroupOrder.removeFirst())
        }
    }

    public func chunks(deviceId: String, sequence: String) async throws -> [DurableRelayChunk] {
        chunkRecords[Self.key(deviceId: deviceId, sequence: sequence), default: []]
    }

    public func putCompleteMessage(_ message: StoredRelayMessage) async throws {
        let key = Self.key(deviceId: message.deviceId, sequence: message.sequence)
        if messages[key] == nil {
            messageOrder.append(key)
        }
        messages[key] = message
        while messages.count > maxCompleteMessages, !messageOrder.isEmpty {
            messages.removeValue(forKey: messageOrder.removeFirst())
        }
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
        messageOrder.removeAll { $0 == key }
        chunkGroupOrder.removeAll { $0 == key }
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
