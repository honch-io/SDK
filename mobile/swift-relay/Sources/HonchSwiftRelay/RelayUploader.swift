import Foundation

public enum RelayUploadOutcome: Equatable, Sendable {
    case consume(status: Int)
    case drop(status: Int?)
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

    private enum FrameOutcome {
        case terminal(RelayUploadOutcome)
        case continueSequence
    }

    public func upload(config: HonchRelayConfig, message: StoredRelayMessage) async -> RelayUploadOutcome {
        // Re-chunk the (possibly oversized) reassembled body into wire-v2 frames
        // and POST them in order: every non-final frame must return 202 (stored,
        // send next) and the final frame returns 204 (complete). Any error
        // mid-sequence ends the attempt and the whole message is retried from the
        // first frame (also satisfying 409 "retry from offset 0").

        // Header values flow straight into the request. Reject control characters
        // (CR/LF/NUL) in the project key and the stream id (derived from the
        // device id) so a crafted value cannot inject headers; drop rather than
        // retry, since the value can never become valid on its own.
        guard isSafeHeaderValue(config.projectKey), isSafeHeaderValue(config.streamId(message)) else {
            return .drop(status: nil)
        }

        let frames: [Data]
        do {
            frames = try WireV2FrameBuilder.buildFrames(
                messageId: config.messageId(message),
                payload: message.body
            )
        } catch {
            // An un-encodable message id can never succeed on retry; drop this one
            // message instead of wedging it (and the drain) in an endless retry.
            return .drop(status: nil)
        }

        let url = captureURL(endpointURL: config.endpointURL)
        for (index, frame) in frames.enumerated() {
            let isFinal = index == frames.count - 1
            let request = makeRequest(config: config, message: message, url: url, frame: frame)

            let response: URLResponse
            do {
                (_, response) = try await session.data(for: request)
            } catch {
                return .retry(status: nil, retryAfterMs: nil)
            }
            guard let httpResponse = response as? HTTPURLResponse else {
                return .retry(status: nil, retryAfterMs: nil)
            }

            switch classify(httpResponse, isFinal: isFinal) {
            case .terminal(let outcome):
                return outcome
            case .continueSequence:
                continue
            }
        }

        // Unreachable: the final frame always yields a terminal outcome.
        return .retry(status: nil, retryAfterMs: nil)
    }

    private func isSafeHeaderValue(_ value: String) -> Bool {
        // HTTP header values must not carry control characters; CR/LF in
        // particular would allow header injection.
        return value.unicodeScalars.allSatisfy { $0.value >= 0x20 && $0.value != 0x7f }
    }

    private func makeRequest(config: HonchRelayConfig, message: StoredRelayMessage, url: URL, frame: Data) -> URLRequest {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.httpBody = frame
        request.setValue("application/vnd.honch.chunk", forHTTPHeaderField: "Content-Type")
        request.setValue(config.projectKey, forHTTPHeaderField: "X-Honch-Project-Key")
        request.setValue(config.streamId(message), forHTTPHeaderField: "X-Honch-Stream-Id")
        request.setValue(config.relayId, forHTTPHeaderField: "X-Honch-Relay-Id")
        request.setValue(config.relaySdkPlatform, forHTTPHeaderField: "X-Honch-Relay-SDK-Platform")
        request.setValue(config.relaySdkVersion, forHTTPHeaderField: "X-Honch-Relay-SDK-Version")
        return request
    }

    private func classify(_ response: HTTPURLResponse, isFinal: Bool) -> FrameOutcome {
        if isFinal {
            if response.statusCode == 204 {
                return .terminal(.consume(status: response.statusCode))
            }
        } else if response.statusCode == 202 {
            return .continueSequence
        }
        // Permanent rejections (matching the C SDK status mapping): 400/401/404/
        // 413/415/422. 413 (too large) is permanent: the relay already re-chunks
        // to a fixed frame size, so retrying the identical bytes can never clear
        // it. 409/429/5xx and any out-of-sequence 202/204 fall through to retry.
        switch response.statusCode {
        case 400, 401, 404, 413, 415, 422:
            return .terminal(.drop(status: response.statusCode))
        default:
            return .terminal(.retry(
                status: response.statusCode,
                retryAfterMs: RetryPolicy.parseRetryAfterMs(response.value(forHTTPHeaderField: "Retry-After"))
            ))
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
