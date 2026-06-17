import Foundation

public enum RelayFrameDecoder {
    private static let headerSize = 20
    private static let sourceTypeEvents: UInt8 = 1

    public static func decode(_ bytes: Data) throws -> RelayFrame {
        guard bytes.count >= headerSize else {
            throw RelayError.frameTooShort
        }

        let version = bytes[0]
        guard version == 1 else {
            throw RelayError.unsupportedFrameVersion(version)
        }

        let sourceType = bytes[1]
        guard sourceType == sourceTypeEvents else {
            throw RelayError.unsupportedSourceType(sourceType)
        }

        let flags = bytes[2]
        guard bytes[3] == 0 else {
            throw RelayError.reservedByteMustBeZero
        }
        let unknownFlags = flags & ~0x03
        guard unknownFlags == 0 else {
            throw RelayError.unknownFlagBits(unknownFlags)
        }

        let payloadLength = Int(readUInt16BE(bytes, at: 16))
        guard bytes.count == headerSize + payloadLength else {
            throw RelayError.payloadLengthMismatch
        }

        let expectedCrc = readUInt16BE(bytes, at: 18)
        let crcInput = bytes.prefix(18) + bytes.dropFirst(headerSize)
        let actualCrc = crc16CcittFalse(crcInput)
        guard actualCrc == expectedCrc else {
            throw RelayError.crcMismatch
        }

        let first = flags & 0x01 != 0
        let offset = readUInt32BE(bytes, at: 12)
        // bit 0 marks the first chunk (spec relay-chunks.md), i.e. the frame at
        // offset 0. Validate the flag rather than decoding it into a field
        // nothing checks: it must be set exactly when offset is zero.
        if first != (offset == 0) {
            throw RelayError.firstFlagOffsetMismatch
        }
        return RelayFrame(
            version: version,
            sourceType: sourceType,
            first: first,
            final: flags & 0x02 != 0,
            sequence: readUInt64BE(bytes, at: 4),
            offset: offset,
            payload: Data(bytes.dropFirst(headerSize))
        )
    }

    public static func crc16CcittFalse(_ bytes: Data) -> UInt16 {
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

    private static func readUInt16BE(_ bytes: Data, at offset: Int) -> UInt16 {
        (UInt16(bytes[offset]) << 8) | UInt16(bytes[offset + 1])
    }

    private static func readUInt32BE(_ bytes: Data, at offset: Int) -> UInt32 {
        (UInt32(bytes[offset]) << 24)
            | (UInt32(bytes[offset + 1]) << 16)
            | (UInt32(bytes[offset + 2]) << 8)
            | UInt32(bytes[offset + 3])
    }

    private static func readUInt64BE(_ bytes: Data, at offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for index in offset..<(offset + 8) {
            value = (value << 8) | UInt64(bytes[index])
        }
        return value
    }
}
