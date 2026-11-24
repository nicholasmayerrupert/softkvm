import Foundation

/// Wire protocol v0. See README.md. All integers little-endian.
/// Every message: u8 type | u8 stream | u16 reserved | u32 payload_len | payload
enum Proto {
    static let version: UInt32 = 0
    static let port: UInt16 = 47900
    static let headerSize = 8
    static let maxPayload = 64 * 1024 * 1024

    enum MsgType: UInt8 {
        // host -> client
        case helloAck = 0x01
        case streamInfo = 0x02
        case video = 0x03
        // client -> host
        case hello = 0x81
        case keyframeRequest = 0x82
        case input = 0x90
    }

    enum Codec: UInt32 {
        case hevc = 1
        case h264 = 2
    }

    struct DisplayRequest: Equatable {
        var width: UInt32
        var height: UInt32
        var fps: UInt32
    }

    static func message(_ type: MsgType, stream: UInt8 = 0, payload: Data = Data()) -> Data {
        var d = Data(capacity: headerSize + payload.count)
        d.append(type.rawValue)
        d.append(stream)
        d.appendLE(UInt16(0))
        d.appendLE(UInt32(payload.count))
        d.append(payload)
        return d
    }
}

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }

    func readLE<T: FixedWidthInteger>(_ type: T.Type, at offset: Int) -> T? {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= count else { return nil }
        var v: T = 0
        _ = Swift.withUnsafeMutableBytes(of: &v) { dst in
            copyBytes(to: dst, from: (startIndex + offset)..<(startIndex + offset + size))
        }
        return T(littleEndian: v)
    }
}
