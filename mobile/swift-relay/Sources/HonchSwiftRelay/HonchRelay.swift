import Foundation

public struct StoredRelayMessage: Equatable, Sendable {
    public let deviceId: String
    public let sourceType: UInt8
    public let sequence: String
    public let body: Data
    public var retryAttempt: Int?
    public var nextAttemptAtMs: Int64?

    public init(
        deviceId: String,
        sourceType: UInt8,
        sequence: String,
        body: Data,
        retryAttempt: Int? = nil,
        nextAttemptAtMs: Int64? = nil
    ) {
        self.deviceId = deviceId
        self.sourceType = sourceType
        self.sequence = sequence
        self.body = body
        self.retryAttempt = retryAttempt
        self.nextAttemptAtMs = nextAttemptAtMs
    }
}

public struct RelayAcknowledgement: Equatable, Sendable {
    public let deviceId: String
    public let sequence: String
    public let ackBytes: Data
    public let message: StoredRelayMessage
}

public struct RelayFrameReceipt: Equatable, Sendable {
    public let complete: Bool
    public let message: StoredRelayMessage?
    public let ackBytes: Data?
}

public struct HonchRelayConfig: Sendable {
    public let endpointURL: URL
    public let projectKey: String
    public let relayId: String
    public let relaySdkPlatform: String
    public let relaySdkVersion: String
    public let streamId: @Sendable (StoredRelayMessage) -> String
    public let messageId: @Sendable (StoredRelayMessage) -> UInt64

    public init(
        endpointURL: URL = URL(string: "http://i.honch.io")!,
        projectKey: String,
        relayId: String,
        relaySdkPlatform: String = "ios",
        relaySdkVersion: String,
        streamId: @escaping @Sendable (StoredRelayMessage) -> String,
        messageId: @escaping @Sendable (StoredRelayMessage) -> UInt64
    ) {
        self.endpointURL = endpointURL
        self.projectKey = projectKey
        self.relayId = relayId
        self.relaySdkPlatform = relaySdkPlatform
        self.relaySdkVersion = relaySdkVersion
        self.streamId = streamId
        self.messageId = messageId
    }
}

public actor HonchRelay {
    private let queue: RelayQueue
    private let config: HonchRelayConfig
    private let uploader: any RelayUploading
    private let scheduler: (any RelayScheduling)?
    private let nowMs: @Sendable () -> Int64
    private let random: @Sendable () -> Double

    public init(
        store: any RelayDurableStore,
        config: HonchRelayConfig,
        uploader: any RelayUploading = URLSessionRelayUploader(),
        scheduler: (any RelayScheduling)? = nil,
        nowMs: @escaping @Sendable () -> Int64 = {
            Int64(Date().timeIntervalSince1970 * 1_000)
        },
        random: @escaping @Sendable () -> Double = {
            Double.random(in: 0..<1)
        }
    ) {
        self.queue = RelayQueue(store: store)
        self.config = config
        self.uploader = uploader
        self.scheduler = scheduler
        self.nowMs = nowMs
        self.random = random
    }

    public func receiveFrame(
        deviceId: String,
        frameBytes: Data,
        acknowledge: (@Sendable (RelayAcknowledgement) async throws -> Void)? = nil
    ) async throws -> RelayFrameReceipt {
        let result = try await queue.putChunk(deviceId: deviceId, frameBytes: frameBytes)
        guard result.complete, let message = result.message else {
            return RelayFrameReceipt(complete: false, message: nil, ackBytes: nil)
        }

        let ackBytes = try RelayAck.build(sequence: message.sequence)
        let acknowledgement = RelayAcknowledgement(
            deviceId: deviceId,
            sequence: message.sequence,
            ackBytes: ackBytes,
            message: message
        )
        if let acknowledge {
            try await acknowledge(acknowledgement)
        }
        return RelayFrameReceipt(complete: true, message: message, ackBytes: ackBytes)
    }

    public func pending() async throws -> [StoredRelayMessage] {
        return try await queue.pending()
    }

    public func startUploadScheduler() async throws {
        if let scheduler {
            try await scheduler.schedule(afterMs: 0)
        }
        try await drainUploads()
    }

    public func stopUploadScheduler() async throws {
        try await scheduler?.cancel()
    }

    public func drainUploads() async throws {
        let drainStartedAtMs = nowMs()
        let messages = try await queue.pending()

        for message in messages {
            if let nextAttemptAtMs = message.nextAttemptAtMs,
               nextAttemptAtMs > drainStartedAtMs {
                continue
            }

            let outcome = await uploader.upload(config: config, message: message)
            switch outcome {
            case .consume:
                try await queue.markUploaded(deviceId: message.deviceId, sequence: message.sequence)
            case .drop:
                try await queue.markDropped(deviceId: message.deviceId, sequence: message.sequence)
            case let .retry(_, retryAfterMs):
                let previousAttempt = message.retryAttempt ?? 0
                let delayMs = retryAfterMs ?? RetryPolicy.nextBackoffDelayMs(
                    attempt: previousAttempt,
                    random: random
                )
                try await queue.markRetry(
                    deviceId: message.deviceId,
                    sequence: message.sequence,
                    retry: RelayRetryState(
                        attempt: previousAttempt + 1,
                        nextAttemptAtMs: drainStartedAtMs + delayMs
                    )
                )
            }
        }

        let pending = try await queue.pending()
        let dueTimes = pending.compactMap(\.nextAttemptAtMs)
        if let earliestDueTime = dueTimes.min() {
            try await scheduler?.schedule(afterMs: max(0, earliestDueTime - drainStartedAtMs))
        }
    }
}
