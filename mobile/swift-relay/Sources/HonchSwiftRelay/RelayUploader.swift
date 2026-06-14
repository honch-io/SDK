import Foundation

public enum RelayUploadOutcome: Equatable, Sendable {
    case consume(status: Int)
    case drop(status: Int)
    case retry(status: Int?, retryAfterMs: Int64?)
}

public protocol RelayUploading: Sendable {
    func upload(config: HonchRelayConfig, message: StoredRelayMessage) async -> RelayUploadOutcome
}

public struct URLSessionRelayUploader: RelayUploading {
    private let session: URLSession

    public init(session: URLSession = .shared) {
        self.session = session
    }

    public func upload(config: HonchRelayConfig, message: StoredRelayMessage) async -> RelayUploadOutcome {
        let body: Data
        do {
            body = try WireV2FrameBuilder.buildSingleFrame(
                messageId: config.messageId(message),
                payload: message.body
            )
        } catch {
            return .retry(status: nil, retryAfterMs: nil)
        }

        var request = URLRequest(url: captureURL(endpointURL: config.endpointURL))
        request.httpMethod = "POST"
        request.httpBody = body
        request.setValue("application/vnd.honch.chunk", forHTTPHeaderField: "Content-Type")
        request.setValue(config.projectKey, forHTTPHeaderField: "X-Honch-Project-Key")
        request.setValue(config.streamId(message), forHTTPHeaderField: "X-Honch-Stream-Id")
        request.setValue(config.relayId, forHTTPHeaderField: "X-Honch-Relay-Id")
        request.setValue(config.relaySdkPlatform, forHTTPHeaderField: "X-Honch-Relay-SDK-Platform")
        request.setValue(config.relaySdkVersion, forHTTPHeaderField: "X-Honch-Relay-SDK-Version")

        let response: URLResponse
        do {
            (_, response) = try await session.data(for: request)
        } catch {
            return .retry(status: nil, retryAfterMs: nil)
        }

        guard let httpResponse = response as? HTTPURLResponse else {
            return .retry(status: nil, retryAfterMs: nil)
        }

        switch httpResponse.statusCode {
        case 204:
            return .consume(status: httpResponse.statusCode)
        case 400, 401, 404:
            return .drop(status: httpResponse.statusCode)
        default:
            return .retry(
                status: httpResponse.statusCode,
                retryAfterMs: RetryPolicy.parseRetryAfterMs(httpResponse.value(forHTTPHeaderField: "Retry-After"))
            )
        }
    }

    private func captureURL(endpointURL: URL) -> URL {
        let urlString = endpointURL
            .absoluteString
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            .appending("/capture")
        return URL(string: urlString)!
    }
}
