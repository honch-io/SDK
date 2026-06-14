public protocol RelayScheduling: Sendable {
    func schedule(afterMs: Int64) async throws
    func cancel() async throws
}
