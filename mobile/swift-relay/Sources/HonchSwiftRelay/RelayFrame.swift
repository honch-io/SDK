import Foundation

public struct RelayFrame: Equatable, Sendable {
    public let version: UInt8
    public let sourceType: UInt8
    public let first: Bool
    public let final: Bool
    public let sequence: UInt64
    public let offset: UInt32
    public let payload: Data
}
