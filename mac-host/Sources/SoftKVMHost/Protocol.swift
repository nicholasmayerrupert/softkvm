import Foundation

/// Messages carried inside the encrypted channel (see Security.swift and docs/PROTOCOL.md).
/// All integers little-endian. Every message: u8 type | u8 stream | u16 reserved | u32 payload_len | payload
enum Proto {
    static let version: UInt32 = UInt32(Handshake.protocolVersion)
    /// TCP port. SOFTKVM_PORT overrides it (e.g. for test instances next to a live one).
    static let port: UInt16 = UInt16(ProcessInfo.processInfo.environment["SOFTKVM_PORT"] ?? "") ?? 47900
    /// SOFTKVM_HEADLESS=1: no menu-bar item and no LAN discovery (test/agent instances).
    static let headless = ProcessInfo.processInfo.environment["SOFTKVM_HEADLESS"] == "1"
    /// UDP port for LAN discovery: clients broadcast `discoverRequest`, hosts answer
    /// `discoverReply` + u16 TCP port + UTF-8 name.
    static let discoveryPort: UInt16 = 47901
    static let discoverRequest = Array("SOFTKVM_DISCOVER".utf8)
    static let discoverReply = Array("SOFTKVM_HOST".utf8)
    static let headerSize = 8
    static let maxPayload = 64 * 1024 * 1024
    static let audioSampleRate = 48_000

    enum MsgType: UInt8 {
        // host -> client
        case helloAck = 0x01
        case streamInfo = 0x02
        case video = 0x03
        case audio = 0x04
        case ping = 0x05
        case layout = 0x06
        case cursor = 0x07
        case cursorPos = 0x08
        case setMonitors = 0x09
        /// Deliberate disconnect: u8 ByeReason, str text. The client should not reconnect on its own.
        case bye = 0x0A
        /// u8 AuthCode, str text. First message after the handshake.
        case authStatus = 0x0B
        // client -> host
        case hello = 0x81
        case keyframeRequest = 0x82
        case monitors = 0x83
        /// The user chose to take control from the PC currently using this Mac.
        case takeover = 0x84
        /// The client is leaving on purpose: u8 reason (0 quitting, 1 switching to another device).
        case goodbye = 0x85
        case input = 0x90
    }

    enum AuthCode: UInt8 {
        case ok = 0               // trusted; send HELLO
        case pairingPending = 1   // waiting for the user to approve on the Mac (compare the code)
        case pairingDenied = 2
        case busy = 3             // text = the PC currently in control; send TAKEOVER to take over
        case paired = 4           // just approved (informational; OK or BUSY follows)
    }

    enum ByeReason: UInt8 {
        case userDisconnected = 0
        case takenOver = 1        // text = the PC that took over
        case unpaired = 2         // the Mac forgot this PC
        case hostQuitting = 3
    }

    static func stringPayload(_ code: UInt8, _ text: String) -> Data {
        var d = Data([code])
        let b = Array(text.utf8.prefix(255))
        d.append(UInt8(b.count))
        d.append(contentsOf: b)
        return d
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
