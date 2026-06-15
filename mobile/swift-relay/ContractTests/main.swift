import Foundation
import HonchSwiftRelay

@main
struct ContractTests {
    static func main() async throws {
        try await testInitialRelayReturnsEmptyPendingMessages()
        try testDecodesValidSingleRelayFrame()
        try testRejectsUnsupportedRelayFrameVersion()
        try testRejectsUnsupportedRelayFrameSourceType()
        try testRejectsRelayFrameUnknownFlagBits()
        try testRejectsRelayFrameReservedByte()
        try testRejectsRelayFramePayloadLengthMismatch()
        try testRejectsRelayFrameCrcMismatch()
        try testBuildsAckWithVersionAndBigEndianSequence()
        try testRejectsAckSequenceOutsideUInt64()
        try await testSingleCompleteFrameCreatesPendingMessage()
        try await testTwoFrameMessageCreatesPendingMessage()
        try await testExactDuplicateCompleteFrameReturnsExistingMessage()
        try await testDuplicateMismatchedFrameThrows()
        try await testConflictingSourceTypeThrows()
        try await testConflictingFinalLengthThrows()
        try await testGapBeforeFinalFrameDoesNotCompleteMessage()
        try await testFileStoreCompletesMessageAcrossRestart()
        try await testFileStorePersistsRetryMetadataAcrossRestart()
        try await testFileStoreDeleteRemovesMessageAndChunks()
        try testRelayConfigDefaultsToSecureIngestEndpoint()
        try testBuildsSingleWireV2UploadFrame()
        try testEncodesMultiByteWireV2MessageId()
        try testRetryPolicyParsesNumericRetryAfter()
        try await testUploaderPostsCaptureRequestAndConsumes204()
        try await testUploaderDropsPermanentRejections()
        try await testUploaderRetriesServerErrorsWithRetryAfter()
        try await testUploaderRetriesNetworkErrors()
        try await testDrainConsumesUploadedMessages()
        try await testDrainDropsPermanentFailures()
        try await testDrainMarksRetryAndSchedulesEarliestRetry()
        try await testDrainSkipsFutureRetryMessages()
    }
}

private func testInitialRelayReturnsEmptyPendingMessages() async throws {
    let store = MemoryRelayStore()
    let relay = HonchRelay(
        store: store,
        config: HonchRelayConfig(
            endpointURL: URL(string: "https://capture.example.test")!,
            projectKey: "project-key",
            relayId: "relay-1",
            relaySdkVersion: "0.1.0",
            streamId: { "relay-\($0.deviceId)" },
            messageId: { UInt64($0.sequence) ?? 0 }
        )
    )

    let pending = try await relay.pending()
    try expectEqual(pending, [StoredRelayMessage](), "new relay has no pending messages")
}

private func expectEqual<T: Equatable>(_ actual: T, _ expected: T, _ message: String) throws {
    if actual != expected {
        throw ContractTestFailure("\(message): expected \(expected), got \(actual)")
    }
}

private func testRelayConfigDefaultsToSecureIngestEndpoint() throws {
    let config = HonchRelayConfig(
        projectKey: "project-key",
        relayId: "relay-1",
        relaySdkVersion: "0.1.0",
        streamId: { "relay-\($0.deviceId)" },
        messageId: { UInt64($0.sequence) ?? 0 }
    )

    try expectEqual(config.endpointURL, URL(string: "https://i.honch.io")!, "default endpoint")
}

private func testDecodesValidSingleRelayFrame() throws {
    let bytes = makeRelayFrame(first: true, final: true, sequence: 7, payload: Data([1, 2, 3]))
    let frame = try RelayFrameDecoder.decode(bytes)

    try expectEqual(frame.version, 1, "frame version")
    try expectEqual(frame.sourceType, 1, "frame source type")
    try expectEqual(frame.first, true, "frame first flag")
    try expectEqual(frame.final, true, "frame final flag")
    try expectEqual(frame.sequence, 7, "frame sequence")
    try expectEqual(frame.offset, 0, "frame offset")
    try expectEqual(frame.payload, Data([1, 2, 3]), "frame payload")
}

private func testRejectsUnsupportedRelayFrameVersion() throws {
    var bytes = makeRelayFrame(first: true, final: true, payload: Data())
    bytes[0] = 2
    try expectThrows(RelayError.unsupportedFrameVersion(2), "unsupported version") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testRejectsUnsupportedRelayFrameSourceType() throws {
    var bytes = makeRelayFrame(first: true, final: true, sourceType: 2, payload: Data())
    writeRelayFrameCrc(&bytes)
    try expectThrows(RelayError.unsupportedSourceType(2), "unsupported source type") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testRejectsRelayFrameUnknownFlagBits() throws {
    var bytes = makeRelayFrame(first: true, final: false, payload: Data([1]))
    bytes[2] = 0x04
    writeRelayFrameCrc(&bytes)
    try expectThrows(RelayError.unknownFlagBits(0x04), "unknown flag bits") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testRejectsRelayFrameReservedByte() throws {
    var bytes = makeRelayFrame(first: true, final: true, payload: Data())
    bytes[3] = 1
    writeRelayFrameCrc(&bytes)
    try expectThrows(RelayError.reservedByteMustBeZero, "reserved byte") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testRejectsRelayFramePayloadLengthMismatch() throws {
    var bytes = makeRelayFrame(first: true, final: true, payload: Data([1, 2]))
    bytes.append(3)
    try expectThrows(RelayError.payloadLengthMismatch, "payload length mismatch") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testRejectsRelayFrameCrcMismatch() throws {
    var bytes = makeRelayFrame(first: true, final: true, payload: Data([1, 2, 3]))
    bytes[18] = 0
    bytes[19] = 0
    try expectThrows(RelayError.crcMismatch, "crc mismatch") {
        _ = try RelayFrameDecoder.decode(bytes)
    }
}

private func testBuildsAckWithVersionAndBigEndianSequence() throws {
    let ack = try RelayAck.build(sequence: "7")
    try expectEqual(ack, Data([1, 0, 0, 0, 0, 0, 0, 0, 7]), "ack bytes")
}

private func testRejectsAckSequenceOutsideUInt64() throws {
    try expectThrows(RelayError.ackSequenceOutOfRange, "ack sequence range") {
        _ = try RelayAck.build(sequence: "18446744073709551616")
    }
}

private func testSingleCompleteFrameCreatesPendingMessage() async throws {
    let relay = makeRelay()
    let frame = makeRelayFrame(first: true, final: true, sequence: 7, payload: Data([9]))

    let receipt = try await relay.receiveFrame(deviceId: "device-a", frameBytes: frame)
    let pending = try await relay.pending()

    try expectEqual(receipt.complete, true, "single frame completes")
    try expectEqual(receipt.ackBytes, Data([1, 0, 0, 0, 0, 0, 0, 0, 7]), "single frame ack")
    try expectEqual(receipt.message?.body, Data([9]), "single frame body")
    try expectEqual(pending.map(\.sequence), ["7"], "single frame pending sequence")
}

private func testTwoFrameMessageCreatesPendingMessage() async throws {
    let relay = makeRelay()
    let first = makeRelayFrame(first: true, final: false, sequence: 8, offset: 0, payload: Data([1, 2]))
    let final = makeRelayFrame(first: false, final: true, sequence: 8, offset: 2, payload: Data([3, 4]))

    let firstReceipt = try await relay.receiveFrame(deviceId: "device-a", frameBytes: first)
    let finalReceipt = try await relay.receiveFrame(deviceId: "device-a", frameBytes: final)

    try expectEqual(firstReceipt.complete, false, "first frame incomplete")
    try expectEqual(finalReceipt.complete, true, "final frame completes")
    try expectEqual(finalReceipt.message?.body, Data([1, 2, 3, 4]), "two frame body")
}

private func testExactDuplicateCompleteFrameReturnsExistingMessage() async throws {
    let relay = makeRelay()
    let frame = makeRelayFrame(first: true, final: true, sequence: 9, payload: Data([5]))

    _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: frame)
    let duplicate = try await relay.receiveFrame(deviceId: "device-a", frameBytes: frame)

    try expectEqual(duplicate.complete, true, "duplicate complete frame completes")
    try expectEqual(duplicate.message?.body, Data([5]), "duplicate complete body")
}

private func testDuplicateMismatchedFrameThrows() async throws {
    let relay = makeRelay()
    let first = makeRelayFrame(first: true, final: false, sequence: 10, offset: 0, payload: Data([1, 2]))
    let conflict = makeRelayFrame(first: true, final: false, sequence: 10, offset: 0, payload: Data([8, 8]))

    _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: first)
    try await expectThrowsAsync(RelayError.duplicateChunkMismatch, "duplicate mismatch") {
        _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: conflict)
    }
}

private func testConflictingSourceTypeThrows() async throws {
    let store = MemoryRelayStore()
    let relay = makeRelay(store: store)
    let first = makeRelayFrame(first: true, final: false, sourceType: 1, sequence: 11, payload: Data([1]))
    let conflict = DurableRelayChunk(
        deviceId: "device-a",
        sourceType: 2,
        sequence: "11",
        offset: 1,
        frameBytes: Data([1]),
        payload: Data([1])
    )

    try await store.putChunk(conflict)
    try await expectThrowsAsync(RelayError.sourceTypeMismatch, "source type mismatch") {
        _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: first)
    }
}

private func testConflictingFinalLengthThrows() async throws {
    let store = MemoryRelayStore()
    let relay = makeRelay(store: store)
    let frame = makeRelayFrame(first: false, final: true, sequence: 12, offset: 2, payload: Data([3]))
    let conflict = DurableRelayChunk(
        deviceId: "device-a",
        sourceType: 1,
        sequence: "12",
        offset: 0,
        frameBytes: Data([1]),
        payload: Data([1, 2]),
        finalEnd: 4
    )

    try await store.putChunk(conflict)
    try await expectThrowsAsync(RelayError.finalChunkLengthMismatch, "final length mismatch") {
        _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: frame)
    }
}

private func testGapBeforeFinalFrameDoesNotCompleteMessage() async throws {
    let relay = makeRelay()
    let final = makeRelayFrame(first: false, final: true, sequence: 13, offset: 2, payload: Data([3]))

    let receipt = try await relay.receiveFrame(deviceId: "device-a", frameBytes: final)

    try expectEqual(receipt.complete, false, "gapped final frame is incomplete")
}

private func testFileStoreCompletesMessageAcrossRestart() async throws {
    let root = try temporaryDirectory()
    let firstStore = FileRelayStore(rootDirectory: root)
    let firstRelay = makeRelay(store: firstStore)
    let first = makeRelayFrame(first: true, final: false, sequence: 21, offset: 0, payload: Data([1, 2]))
    let final = makeRelayFrame(first: false, final: true, sequence: 21, offset: 2, payload: Data([3, 4]))

    let firstReceipt = try await firstRelay.receiveFrame(deviceId: "device-a", frameBytes: first)
    let secondStore = FileRelayStore(rootDirectory: root)
    let secondRelay = makeRelay(store: secondStore)
    let finalReceipt = try await secondRelay.receiveFrame(deviceId: "device-a", frameBytes: final)
    let pending = try await secondRelay.pending()

    try expectEqual(firstReceipt.complete, false, "file store first frame incomplete")
    try expectEqual(finalReceipt.complete, true, "file store final completes after restart")
    try expectEqual(finalReceipt.message?.body, Data([1, 2, 3, 4]), "file store reassembled body")
    try expectEqual(pending.map(\.sequence), ["21"], "file store pending after restart")
}

private func testFileStorePersistsRetryMetadataAcrossRestart() async throws {
    let root = try temporaryDirectory()
    let store = FileRelayStore(rootDirectory: root)
    let message = StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "22", body: Data([1]))

    try await store.putCompleteMessage(message)
    try await store.markRetry(
        deviceId: "device-a",
        sequence: "22",
        retry: RelayRetryState(attempt: 3, nextAttemptAtMs: 123_456)
    )
    let restarted = FileRelayStore(rootDirectory: root)
    let pending = try await restarted.completeMessages()

    try expectEqual(pending.first?.retryAttempt, 3, "file store retry attempt")
    try expectEqual(pending.first?.nextAttemptAtMs, 123_456, "file store retry next attempt")
}

private func testFileStoreDeleteRemovesMessageAndChunks() async throws {
    let root = try temporaryDirectory()
    let store = FileRelayStore(rootDirectory: root)
    let relay = makeRelay(store: store)
    let frame = makeRelayFrame(first: true, final: true, sequence: 23, payload: Data([9]))

    _ = try await relay.receiveFrame(deviceId: "device-a", frameBytes: frame)
    try await store.deleteMessage(deviceId: "device-a", sequence: "23")

    try expectEqual(try await store.completeMessages(), [StoredRelayMessage](), "file store deleted messages")
    try expectEqual(try await store.chunks(deviceId: "device-a", sequence: "23"), [DurableRelayChunk](), "file store deleted chunks")
}

private func testBuildsSingleWireV2UploadFrame() throws {
    let frame = try WireV2FrameBuilder.buildSingleFrame(messageId: 7, payload: Data([1, 2, 3]))
    let crc = TestCrc.crc16CcittFalse(Data([1, 2, 3]))

    try expectEqual(frame.first, 0x02, "wire-v2 header")
    try expectEqual(frame[1], 0x07, "wire-v2 message id")
    try expectEqual(Data(frame[2..<5]), Data([1, 2, 3]), "wire-v2 payload")
    try expectEqual(frame[5], UInt8(crc & 0xff), "wire-v2 crc low byte")
    try expectEqual(frame[6], UInt8((crc >> 8) & 0xff), "wire-v2 crc high byte")
}

private func testEncodesMultiByteWireV2MessageId() throws {
    let frame = try WireV2FrameBuilder.buildSingleFrame(messageId: 300, payload: Data())

    try expectEqual(Data(frame.prefix(3)), Data([0x02, 0xac, 0x02]), "wire-v2 uvarint message id")
}

private func testRetryPolicyParsesNumericRetryAfter() throws {
    try expectEqual(RetryPolicy.parseRetryAfterMs("2"), 2_000, "numeric retry-after")
}

private func testUploaderPostsCaptureRequestAndConsumes204() async throws {
    let message = StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "7", body: Data([1, 2, 3]))
    let expectedBody = try WireV2FrameBuilder.buildSingleFrame(messageId: 7, payload: message.body)
    let session = makeStubbedURLSession { request in
        try expectEqual(request.url?.absoluteString, "https://capture.example.test/capture", "capture URL")
        try expectEqual(request.httpMethod, "POST", "capture method")
        try expectEqual(request.value(forHTTPHeaderField: "Content-Type"), "application/vnd.honch.chunk", "content type")
        try expectEqual(request.value(forHTTPHeaderField: "X-Honch-Project-Key"), "project-key", "project key header")
        try expectEqual(request.value(forHTTPHeaderField: "X-Honch-Stream-Id"), "relay-device-a", "stream header")
        try expectEqual(request.value(forHTTPHeaderField: "X-Honch-Relay-Id"), "relay-1", "relay id header")
        try expectEqual(request.value(forHTTPHeaderField: "X-Honch-Relay-SDK-Platform"), "ios", "relay platform header")
        try expectEqual(request.value(forHTTPHeaderField: "X-Honch-Relay-SDK-Version"), "0.1.0", "relay version header")
        try expectEqual(try requestBody(request), expectedBody, "capture body")
        return (204, [:], Data())
    }
    let uploader = URLSessionRelayUploader(session: session)

    let outcome = await uploader.upload(config: makeConfig(), message: message)

    try expectEqual(outcome, .consume(status: 204), "204 consumes upload")
}

private func testUploaderDropsPermanentRejections() async throws {
    for status in [400, 401, 404] {
        let session = makeStubbedURLSession { _ in (status, [:], Data()) }
        let uploader = URLSessionRelayUploader(session: session)
        let outcome = await uploader.upload(
            config: makeConfig(),
            message: StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "7", body: Data([1]))
        )

        try expectEqual(outcome, .drop(status: status), "\(status) drops upload")
    }
}

private func testUploaderRetriesServerErrorsWithRetryAfter() async throws {
    let session = makeStubbedURLSession { _ in (500, ["Retry-After": "2"], Data()) }
    let uploader = URLSessionRelayUploader(session: session)

    let outcome = await uploader.upload(
        config: makeConfig(),
        message: StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "7", body: Data([1]))
    )

    try expectEqual(outcome, .retry(status: 500, retryAfterMs: 2_000), "500 retries with retry-after")
}

private func testUploaderRetriesNetworkErrors() async throws {
    let session = makeStubbedURLSession { _ in throw URLError(.notConnectedToInternet) }
    let uploader = URLSessionRelayUploader(session: session)

    let outcome = await uploader.upload(
        config: makeConfig(),
        message: StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "7", body: Data([1]))
    )

    try expectEqual(outcome, .retry(status: nil, retryAfterMs: nil), "network errors retry")
}

private func testDrainConsumesUploadedMessages() async throws {
    let store = MemoryRelayStore()
    try await store.putCompleteMessage(StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "1", body: Data([1])))
    let relay = HonchRelay(
        store: store,
        config: makeConfig(),
        uploader: RecordingUploader(outcomes: [.consume(status: 204)]),
        nowMs: { 1_000 }
    )

    try await relay.drainUploads()

    try expectEqual(try await relay.pending(), [StoredRelayMessage](), "consume removes message")
}

private func testDrainDropsPermanentFailures() async throws {
    let store = MemoryRelayStore()
    try await store.putCompleteMessage(StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "1", body: Data([1])))
    let relay = HonchRelay(
        store: store,
        config: makeConfig(),
        uploader: RecordingUploader(outcomes: [.drop(status: 400)]),
        nowMs: { 1_000 }
    )

    try await relay.drainUploads()

    try expectEqual(try await relay.pending(), [StoredRelayMessage](), "drop removes message")
}

private func testDrainMarksRetryAndSchedulesEarliestRetry() async throws {
    let store = MemoryRelayStore()
    let scheduler = RecordingScheduler()
    try await store.putCompleteMessage(StoredRelayMessage(deviceId: "device-a", sourceType: 1, sequence: "1", body: Data([1])))
    let relay = HonchRelay(
        store: store,
        config: makeConfig(),
        uploader: RecordingUploader(outcomes: [.retry(status: 500, retryAfterMs: 2_000)]),
        scheduler: scheduler,
        nowMs: { 1_000 }
    )

    try await relay.drainUploads()

    let pending = try await relay.pending()
    try expectEqual(pending.first?.retryAttempt, 1, "retry attempt")
    try expectEqual(pending.first?.nextAttemptAtMs, 3_000, "retry next attempt")
    try expectEqual(await scheduler.scheduledDelays(), [2_000], "scheduled retry delay")
}

private func testDrainSkipsFutureRetryMessages() async throws {
    let store = MemoryRelayStore()
    let uploader = RecordingUploader(outcomes: [.consume(status: 204)])
    try await store.putCompleteMessage(
        StoredRelayMessage(
            deviceId: "device-a",
            sourceType: 1,
            sequence: "1",
            body: Data([1]),
            retryAttempt: 1,
            nextAttemptAtMs: 5_000
        )
    )
    let relay = HonchRelay(
        store: store,
        config: makeConfig(),
        uploader: uploader,
        nowMs: { 1_000 }
    )

    try await relay.drainUploads()

    try expectEqual(await uploader.uploadedSequences(), [String](), "future retry skipped")
    let pendingSequences = try await relay.pending().map { $0.sequence }
    try expectEqual(pendingSequences, ["1"], "future retry remains pending")
}

private func makeRelayFrame(
    first: Bool,
    final: Bool,
    sourceType: UInt8 = 1,
    sequence: UInt64 = 1,
    offset: UInt32 = 0,
    payload: Data
) -> Data {
    var bytes = Data(repeating: 0, count: 20 + payload.count)
    bytes[0] = 1
    bytes[1] = sourceType
    bytes[2] = (first ? 0x01 : 0x00) | (final ? 0x02 : 0x00)
    writeUInt64BE(sequence, to: &bytes, at: 4)
    writeUInt32BE(offset, to: &bytes, at: 12)
    writeUInt16BE(UInt16(payload.count), to: &bytes, at: 16)
    bytes.replaceSubrange(20..<(20 + payload.count), with: payload)
    writeRelayFrameCrc(&bytes)
    return bytes
}

private func writeRelayFrameCrc(_ bytes: inout Data) {
    bytes[18] = 0
    bytes[19] = 0
    let crc = TestCrc.crc16CcittFalse(bytes.prefix(18) + bytes.dropFirst(20))
    writeUInt16BE(crc, to: &bytes, at: 18)
}

private func writeUInt16BE(_ value: UInt16, to bytes: inout Data, at offset: Int) {
    bytes[offset] = UInt8((value >> 8) & 0xff)
    bytes[offset + 1] = UInt8(value & 0xff)
}

private func writeUInt32BE(_ value: UInt32, to bytes: inout Data, at offset: Int) {
    bytes[offset] = UInt8((value >> 24) & 0xff)
    bytes[offset + 1] = UInt8((value >> 16) & 0xff)
    bytes[offset + 2] = UInt8((value >> 8) & 0xff)
    bytes[offset + 3] = UInt8(value & 0xff)
}

private func writeUInt64BE(_ value: UInt64, to bytes: inout Data, at offset: Int) {
    for index in 0..<8 {
        let shift = UInt64((7 - index) * 8)
        bytes[offset + index] = UInt8((value >> shift) & 0xff)
    }
}

private func expectThrows<E: Error & Equatable>(
    _ expected: E,
    _ message: String,
    operation: () throws -> Void
) throws {
    do {
        try operation()
    } catch let error as E {
        try expectEqual(error, expected, message)
        return
    } catch {
        throw ContractTestFailure("\(message): expected \(expected), got \(error)")
    }
    throw ContractTestFailure("\(message): expected \(expected), but no error was thrown")
}

private func expectThrowsAsync<E: Error & Equatable>(
    _ expected: E,
    _ message: String,
    operation: () async throws -> Void
) async throws {
    do {
        try await operation()
    } catch let error as E {
        try expectEqual(error, expected, message)
        return
    } catch {
        throw ContractTestFailure("\(message): expected \(expected), got \(error)")
    }
    throw ContractTestFailure("\(message): expected \(expected), but no error was thrown")
}

private func makeRelay(store: any RelayDurableStore = MemoryRelayStore()) -> HonchRelay {
    HonchRelay(
        store: store,
        config: makeConfig()
    )
}

private func makeConfig() -> HonchRelayConfig {
    HonchRelayConfig(
        endpointURL: URL(string: "https://capture.example.test")!,
        projectKey: "project-key",
        relayId: "relay-1",
        relaySdkVersion: "0.1.0",
        streamId: { "relay-\($0.deviceId)" },
        messageId: { UInt64($0.sequence) ?? 0 }
    )
}

private typealias StubResponse = (status: Int, headers: [String: String], body: Data)
private typealias StubHandler = @Sendable (URLRequest) throws -> StubResponse

private func makeStubbedURLSession(handler: @escaping StubHandler) -> URLSession {
    StubURLProtocol.handler = handler
    let configuration = URLSessionConfiguration.ephemeral
    configuration.protocolClasses = [StubURLProtocol.self]
    return URLSession(configuration: configuration)
}

private func requestBody(_ request: URLRequest) throws -> Data? {
    if let httpBody = request.httpBody {
        return httpBody
    }
    guard let stream = request.httpBodyStream else {
        return nil
    }
    stream.open()
    defer { stream.close() }

    var body = Data()
    let bufferSize = 1024
    let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
    defer { buffer.deallocate() }

    while stream.hasBytesAvailable {
        let count = stream.read(buffer, maxLength: bufferSize)
        if count < 0 {
            throw stream.streamError ?? URLError(.cannotDecodeRawData)
        }
        if count == 0 {
            break
        }
        body.append(buffer, count: count)
    }
    return body
}

private final class StubURLProtocol: URLProtocol {
    nonisolated(unsafe) static var handler: StubHandler?

    override class func canInit(with request: URLRequest) -> Bool {
        true
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest {
        request
    }

    override func startLoading() {
        guard let handler = Self.handler else {
            client?.urlProtocol(self, didFailWithError: URLError(.badServerResponse))
            return
        }
        do {
            let response = try handler(request)
            let httpResponse = HTTPURLResponse(
                url: request.url!,
                statusCode: response.status,
                httpVersion: "HTTP/1.1",
                headerFields: response.headers
            )!
            client?.urlProtocol(self, didReceive: httpResponse, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: response.body)
            client?.urlProtocolDidFinishLoading(self)
        } catch {
            client?.urlProtocol(self, didFailWithError: error)
        }
    }

    override func stopLoading() {}
}

private actor RecordingUploader: RelayUploading {
    private var outcomes: [RelayUploadOutcome]
    private var sequences: [String] = []

    init(outcomes: [RelayUploadOutcome]) {
        self.outcomes = outcomes
    }

    func upload(config: HonchRelayConfig, message: StoredRelayMessage) async -> RelayUploadOutcome {
        _ = config
        sequences.append(message.sequence)
        if outcomes.isEmpty {
            return .consume(status: 204)
        }
        return outcomes.removeFirst()
    }

    func uploadedSequences() -> [String] {
        sequences
    }
}

private actor RecordingScheduler: RelayScheduling {
    private var delays: [Int64] = []
    private var cancelCount = 0

    func schedule(afterMs: Int64) async throws {
        delays.append(afterMs)
    }

    func cancel() async throws {
        cancelCount += 1
    }

    func scheduledDelays() -> [Int64] {
        delays
    }
}

private func temporaryDirectory() throws -> URL {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("honch-swift-relay-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

private enum TestCrc {
    static func crc16CcittFalse(_ bytes: Data) -> UInt16 {
        var crc: UInt16 = 0xffff
        for byte in bytes {
            crc ^= UInt16(byte) << 8
            for _ in 0..<8 {
                if crc & 0x8000 != 0 {
                    crc = (crc << 1) ^ 0x1021
                } else {
                    crc <<= 1
                }
            }
        }
        return crc
    }
}

private struct ContractTestFailure: Error, CustomStringConvertible {
    let description: String

    init(_ description: String) {
        self.description = description
    }
}
