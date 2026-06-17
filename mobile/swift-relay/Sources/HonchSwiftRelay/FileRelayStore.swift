import Foundation

public actor FileRelayStore: RelayDurableStore {
    private let rootDirectory: URL
    private let maxCompleteMessages: Int
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    public init(rootDirectory: URL, maxCompleteMessages: Int = 1024) {
        self.rootDirectory = rootDirectory
        self.maxCompleteMessages = maxCompleteMessages
    }

    public func putChunk(_ chunk: DurableRelayChunk) async throws {
        let url = chunkURL(deviceId: chunk.deviceId, sequence: chunk.sequence, offset: chunk.offset)
        try write(
            ChunkRecord(
                deviceId: chunk.deviceId,
                sourceType: chunk.sourceType,
                sequence: chunk.sequence,
                offset: chunk.offset,
                frameBase64: chunk.frameBytes.base64EncodedString(),
                payloadBase64: chunk.payload.base64EncodedString(),
                finalEnd: chunk.finalEnd
            ),
            to: url
        )
    }

    public func chunks(deviceId: String, sequence: String) async throws -> [DurableRelayChunk] {
        let directory = chunkDirectory(deviceId: deviceId, sequence: sequence)
        guard FileManager.default.fileExists(atPath: directory.path) else {
            return []
        }

        let urls = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: nil
        )
        return try urls
            .filter { $0.pathExtension == "json" }
            .map { try read(ChunkRecord.self, from: $0).chunk() }
            .sorted { $0.offset < $1.offset }
    }

    public func putCompleteMessage(_ message: StoredRelayMessage) async throws {
        let url = messageURL(deviceId: message.deviceId, sequence: message.sequence)
        try write(MessageRecord(message), to: url)
        try enforceMessageLimit()
    }

    // Bound the on-disk store: when more than maxCompleteMessages pending uploads
    // are stored, drop the oldest (by file creation time) along with its chunk
    // directory. Drop-oldest, no time TTL -- matching the other relay stores.
    private func enforceMessageLimit() throws {
        let directory = messagesDirectory()
        guard FileManager.default.fileExists(atPath: directory.path) else {
            return
        }
        var files: [(url: URL, created: Date)] = []
        let deviceDirectories = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.creationDateKey]
        )
        for deviceDirectory in deviceDirectories {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: deviceDirectory.path, isDirectory: &isDirectory),
                  isDirectory.boolValue else {
                continue
            }
            let urls = try FileManager.default.contentsOfDirectory(
                at: deviceDirectory,
                includingPropertiesForKeys: [.creationDateKey]
            )
            for url in urls where url.pathExtension == "json" {
                let created = (try? url.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? Date.distantPast
                files.append((url, created))
            }
        }
        guard files.count > maxCompleteMessages else {
            return
        }
        let oldest = files.sorted { $0.created < $1.created }.prefix(files.count - maxCompleteMessages)
        for entry in oldest {
            // The message lives at messages/<deviceB64>/<seqB64>.json and its
            // chunks at chunks/<deviceB64>/<seqB64>/; remove both.
            let seqComponent = entry.url.deletingPathExtension().lastPathComponent
            let deviceComponent = entry.url.deletingLastPathComponent().lastPathComponent
            let chunkDirectory = rootDirectory
                .appendingPathComponent("chunks", isDirectory: true)
                .appendingPathComponent(deviceComponent, isDirectory: true)
                .appendingPathComponent(seqComponent, isDirectory: true)
            try? FileManager.default.removeItem(at: chunkDirectory)
            try? FileManager.default.removeItem(at: entry.url)
        }
    }

    public func completeMessages() async throws -> [StoredRelayMessage] {
        let directory = messagesDirectory()
        guard FileManager.default.fileExists(atPath: directory.path) else {
            return []
        }

        let deviceDirectories = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: nil
        )
        var messages: [StoredRelayMessage] = []
        for deviceDirectory in deviceDirectories {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: deviceDirectory.path, isDirectory: &isDirectory),
                  isDirectory.boolValue else {
                continue
            }
            let messageURLs = try FileManager.default.contentsOfDirectory(
                at: deviceDirectory,
                includingPropertiesForKeys: nil
            )
            for url in messageURLs where url.pathExtension == "json" {
                messages.append(try read(MessageRecord.self, from: url).message())
            }
        }
        return messages.sorted {
            if $0.deviceId == $1.deviceId {
                return $0.sequence < $1.sequence
            }
            return $0.deviceId < $1.deviceId
        }
    }

    public func deleteMessage(deviceId: String, sequence: String) async throws {
        let messageURL = messageURL(deviceId: deviceId, sequence: sequence)
        if FileManager.default.fileExists(atPath: messageURL.path) {
            try FileManager.default.removeItem(at: messageURL)
        }

        let chunkDirectory = chunkDirectory(deviceId: deviceId, sequence: sequence)
        if FileManager.default.fileExists(atPath: chunkDirectory.path) {
            try FileManager.default.removeItem(at: chunkDirectory)
        }
    }

    public func markRetry(deviceId: String, sequence: String, retry: RelayRetryState) async throws {
        let url = messageURL(deviceId: deviceId, sequence: sequence)
        guard FileManager.default.fileExists(atPath: url.path) else {
            return
        }
        var record = try read(MessageRecord.self, from: url)
        record.retryAttempt = retry.attempt
        record.nextAttemptAtMs = retry.nextAttemptAtMs
        try write(record, to: url)
    }

    private func write<T: Encodable>(_ value: T, to url: URL) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let data = try encoder.encode(value)
        try data.write(to: url, options: [.atomic])
    }

    private func read<T: Decodable>(_ type: T.Type, from url: URL) throws -> T {
        let data = try Data(contentsOf: url)
        return try decoder.decode(type, from: data)
    }

    private func chunkURL(deviceId: String, sequence: String, offset: UInt32) -> URL {
        chunkDirectory(deviceId: deviceId, sequence: sequence)
            .appendingPathComponent(String(format: "%010u", offset))
            .appendingPathExtension("json")
    }

    private func chunkDirectory(deviceId: String, sequence: String) -> URL {
        rootDirectory
            .appendingPathComponent("chunks", isDirectory: true)
            .appendingPathComponent(Self.pathComponent(deviceId), isDirectory: true)
            .appendingPathComponent(Self.pathComponent(sequence), isDirectory: true)
    }

    private func messageURL(deviceId: String, sequence: String) -> URL {
        messagesDirectory()
            .appendingPathComponent(Self.pathComponent(deviceId), isDirectory: true)
            .appendingPathComponent(Self.pathComponent(sequence))
            .appendingPathExtension("json")
    }

    private func messagesDirectory() -> URL {
        rootDirectory.appendingPathComponent("messages", isDirectory: true)
    }

    private static func pathComponent(_ value: String) -> String {
        Data(value.utf8)
            .base64EncodedString()
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "=", with: "")
    }
}

private struct ChunkRecord: Codable {
    let deviceId: String
    let sourceType: UInt8
    let sequence: String
    let offset: UInt32
    let frameBase64: String
    let payloadBase64: String
    let finalEnd: UInt32?

    func chunk() throws -> DurableRelayChunk {
        guard let frameBytes = Data(base64Encoded: frameBase64),
              let payload = Data(base64Encoded: payloadBase64) else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(codingPath: [], debugDescription: "invalid base64 relay chunk")
            )
        }
        return DurableRelayChunk(
            deviceId: deviceId,
            sourceType: sourceType,
            sequence: sequence,
            offset: offset,
            frameBytes: frameBytes,
            payload: payload,
            finalEnd: finalEnd
        )
    }
}

private struct MessageRecord: Codable {
    let deviceId: String
    let sourceType: UInt8
    let sequence: String
    let bodyBase64: String
    var retryAttempt: Int?
    var nextAttemptAtMs: Int64?

    init(_ message: StoredRelayMessage) {
        self.deviceId = message.deviceId
        self.sourceType = message.sourceType
        self.sequence = message.sequence
        self.bodyBase64 = message.body.base64EncodedString()
        self.retryAttempt = message.retryAttempt
        self.nextAttemptAtMs = message.nextAttemptAtMs
    }

    func message() throws -> StoredRelayMessage {
        guard let body = Data(base64Encoded: bodyBase64) else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(codingPath: [], debugDescription: "invalid base64 relay message")
            )
        }
        return StoredRelayMessage(
            deviceId: deviceId,
            sourceType: sourceType,
            sequence: sequence,
            body: body,
            retryAttempt: retryAttempt,
            nextAttemptAtMs: nextAttemptAtMs
        )
    }
}
