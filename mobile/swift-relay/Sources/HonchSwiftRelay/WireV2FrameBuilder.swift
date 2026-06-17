import Foundation

public enum WireV2FrameError: Error {
    case maxFrameSizeTooSmall
    case overheadExceedsFrame
    case nonPositivePayload
}

public enum WireV2FrameBuilder {
    static let headerContinuation: UInt8 = 0x20
    static let headerMore: UInt8 = 0x40
    static let finalCrcSize = 2
    // Matches the shipping C SDK frame size (HONCH_WIRE_V2_MAX_FRAME_BYTES),
    // which Capture is known to accept, so re-chunked relay frames are valid.
    public static let maxFrameSize = 4096

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

    // Split a complete compact message into wire-v2 frames each <= maxFrameSize,
    // so an oversized relayed body is delivered as a multi-frame sequence rather
    // than a single over-limit frame. A payload that fits returns one frame
    // (byte-identical to buildSingleFrame). Faithful port of the C chunker.
    public static func buildFrames(
        messageId: UInt64,
        payload: Data,
        maxFrameSize: Int = WireV2FrameBuilder.maxFrameSize
    ) throws -> [Data] {
        guard maxFrameSize >= 8 else { throw WireV2FrameError.maxFrameSizeTooSmall }
        let total = payload.count
        let crcWhole = RelayFrameDecoder.crc16CcittFalse(payload)

        if total == 0 {
            return [encodeFrame(messageId: messageId, payload: payload, offset: 0, total: 0,
                                continuation: false, more: false, crcWhole: crcWhole)]
        }

        var frames: [Data] = []
        var offset = 0
        repeat {
            let continuation = offset != 0
            let remaining = total - offset

            let finalOverhead = frameOverhead(messageId: messageId, offset: offset, total: total,
                                              continuation: continuation, more: false)
            guard finalOverhead < maxFrameSize else { throw WireV2FrameError.overheadExceedsFrame }
            let finalCapacity = maxFrameSize - finalOverhead

            let more = remaining > finalCapacity
            var payloadCapacity = finalCapacity
            if more {
                let moreOverhead = frameOverhead(messageId: messageId, offset: offset, total: total,
                                                continuation: continuation, more: true)
                guard moreOverhead < maxFrameSize else { throw WireV2FrameError.overheadExceedsFrame }
                payloadCapacity = maxFrameSize - moreOverhead
            }

            var payloadSize = min(remaining, payloadCapacity)
            if more && payloadSize >= remaining { payloadSize = remaining - finalCapacity }
            guard payloadSize > 0 else { throw WireV2FrameError.nonPositivePayload }

            let lower = payload.startIndex + offset
            let chunk = payload.subdata(in: lower ..< (lower + payloadSize))
            frames.append(encodeFrame(messageId: messageId, payload: chunk, offset: offset, total: total,
                                      continuation: continuation, more: more, crcWhole: crcWhole))
            offset += payloadSize
        } while offset < total

        return frames
    }

    static func uvarintSize(_ value: UInt64) -> Int {
        var size = 1
        var remaining = value >> 7
        while remaining != 0 {
            size += 1
            remaining >>= 7
        }
        return size
    }

    static func frameOverhead(messageId: UInt64, offset: Int, total: Int, continuation: Bool, more: Bool) -> Int {
        var overhead = 1 + uvarintSize(messageId)
        if continuation {
            overhead += uvarintSize(UInt64(offset))
        } else if more {
            overhead += uvarintSize(UInt64(total))
        }
        if !more {
            overhead += finalCrcSize
        }
        return overhead
    }

    static func encodeFrame(messageId: UInt64, payload: Data, offset: Int, total: Int,
                            continuation: Bool, more: Bool, crcWhole: UInt16) -> Data {
        var header: UInt8 = 0x02 // version 2, source type events (0)
        if continuation { header |= headerContinuation }
        if more { header |= headerMore }

        var frame = Data()
        frame.append(header)
        frame.append(encodeUvarint(messageId))
        if continuation {
            frame.append(encodeUvarint(UInt64(offset)))
        } else if more {
            frame.append(encodeUvarint(UInt64(total)))
        }
        frame.append(payload)
        if !more {
            // CRC is over the WHOLE reassembled message, not just this chunk.
            frame.append(UInt8(crcWhole & 0xff))
            frame.append(UInt8((crcWhole >> 8) & 0xff))
        }
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
