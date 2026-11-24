import Darwin
import Foundation

/// Latency instrumentation shared with the PC client (see README "Timing").
///
/// All host timestamps are microseconds on the Mac host clock (mach_absolute_time), the same
/// base ScreenCaptureKit uses for frame display times, so the client can line up every stage.
enum HostClock {
    private static let timebase: mach_timebase_info_data_t = {
        var t = mach_timebase_info_data_t()
        mach_timebase_info(&t)
        return t
    }()

    static func micros(fromTicks t: UInt64) -> UInt64 {
        // Apple Silicon: 125/3 ns per tick; no overflow for centuries of uptime.
        t * UInt64(timebase.numer) / UInt64(timebase.denom) / 1000
    }

    static func nowMicros() -> UInt64 { micros(fromTicks: mach_absolute_time()) }
}

/// When each stage of one video frame happened on the host.
struct FrameTiming {
    var captureUs: UInt64 = 0      // ScreenCaptureKit display time (when the frame was composited)
    var callbackUs: UInt64 = 0     // ScreenCaptureKit handed us the frame
    var submitUs: UInt64 = 0       // handed to VideoToolbox (later than callback if the encoder was busy)
    var encodedUs: UInt64 = 0      // VideoToolbox output callback
    var refresh = false            // idle re-encode of an old frame (not a fresh capture)
}

enum TimingProto {
    // Message types (chosen well away from the v0 set so other additions don't collide).
    /// host -> client, `stream` = display: u64 pts_us (matches VIDEO), u64 capture_us,
    /// u64 callback_us, u64 encoded_us, u64 queued_us (handed to the socket), u32 pending_bytes
    /// (unsent bytes ahead of this frame), u32 flags (bit 0 = idle refresh), u64 submit_us.
    static let frameTiming: UInt8 = 0x20
    /// host -> client: u64 client_us (echoed), u64 host_us.
    static let timeResponse: UInt8 = 0x21
    /// client -> host: u64 client_us. Clients only send it once a PING carried a host time.
    static let timeRequest: UInt8 = 0xA0

    static func message(type: UInt8, stream: UInt8 = 0, payload: Data) -> Data {
        var d = Data(capacity: Proto.headerSize + payload.count)
        d.append(type)
        d.append(stream)
        d.appendLE(UInt16(0))
        d.appendLE(UInt32(payload.count))
        d.append(payload)
        return d
    }

    /// PING payload: u64 host_us. Old clients ignore the payload.
    static func pingPayload() -> Data {
        var d = Data()
        d.appendLE(HostClock.nowMicros())
        return d
    }

    static func frameTimingMessage(stream: Int, pts: UInt64, timing t: FrameTiming, pendingBytes: Int) -> Data {
        var d = Data(capacity: 56)
        d.appendLE(pts)
        d.appendLE(t.captureUs)
        d.appendLE(t.callbackUs)
        d.appendLE(t.encodedUs)
        d.appendLE(HostClock.nowMicros())
        d.appendLE(UInt32(clamping: pendingBytes))
        d.appendLE(UInt32(t.refresh ? 1 : 0))
        d.appendLE(t.submitUs)
        return message(type: frameTiming, stream: UInt8(stream), payload: d)
    }

    /// Answers a TIME_REQUEST; returns nil for any other message type.
    static func handle(type: UInt8, payload: Data) -> Data? {
        guard type == timeRequest, let clientUs = payload.readLE(UInt64.self, at: 0) else { return nil }
        var d = Data(capacity: 16)
        d.appendLE(clientUs)
        d.appendLE(HostClock.nowMicros())
        return message(type: timeResponse, payload: d)
    }
}

/// Environment switches for running a second, isolated host next to a live one (benchmarks).
enum HostEnv {
    static let env = ProcessInfo.processInfo.environment

    /// SOFTKVM_PORT: TCP port to listen on (default 47900).
    static let port: UInt16 = Proto.port  // Proto.port already honors SOFTKVM_PORT

    /// SOFTKVM_PERF=1: benchmark instance. Virtual displays are named "SoftKVM perf N" with
    /// distinct serials, the client's monitor arrangement is not mirrored, keyboard/mouse
    /// input from the client is ignored, and the log goes to SoftKVMHost-perf.log.
    static let perf = env["SOFTKVM_PERF"] == "1"

    /// SOFTKVM_ANIMATE=1: put an animated test pattern on each virtual display (benchmarks).
    static let animate = env["SOFTKVM_ANIMATE"] == "1"

    static func displayName(_ index: Int) -> String {
        perf ? "SoftKVM perf \(index + 1)" : Proto.headless ? "SoftKVM test \(index + 1)" : "SoftKVM \(index + 1)"
    }

    static func displaySerial(_ index: Int) -> UInt32 {
        // Test instances must not reuse the live host's serials: macOS refuses a second display
        // with the same vendor/product/serial.
        UInt32(index + 1) + (perf ? 0x5000 : Proto.headless ? 0x4000 : 0)
    }
}
