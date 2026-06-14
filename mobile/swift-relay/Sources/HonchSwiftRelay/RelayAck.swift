import Foundation

public enum RelayAck {
    public static func build(sequence: String) throws -> Data {
        guard let value = UInt64(sequence) else {
            throw RelayError.ackSequenceOutOfRange
        }

        var ack = Data(repeating: 0, count: 9)
        ack[0] = 1
        for index in 0..<8 {
            let shift = UInt64((7 - index) * 8)
            ack[index + 1] = UInt8((value >> shift) & 0xff)
        }
        return ack
    }
}
