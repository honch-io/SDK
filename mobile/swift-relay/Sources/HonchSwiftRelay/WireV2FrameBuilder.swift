import Foundation

public enum WireV2FrameBuilder {
    public static func buildSingleFrame(messageId: UInt64, payload: Data) throws -> Data {
        var frame = Data()
        frame.append(0x02)
        frame.append(encodeUvarint(messageId))
        frame.append(payload)

        let crc = RelayFrameDecoder.crc16CcittFalse(payload)
        frame.append(UInt8(crc & 0xff))
        frame.append(UInt8((crc >> 8) & 0xff))
        return frame
    }

    static func encodeUvarint(_ value: UInt64) -> Data {
        var remaining = value
        var bytes = Data()
        repeat {
            var byte = UInt8(remaining & 0x7f)
            remaining >>= 7
            if remaining != 0 {
                byte |= 0x80
            }
            bytes.append(byte)
        } while remaining != 0
        return bytes
    }
}
