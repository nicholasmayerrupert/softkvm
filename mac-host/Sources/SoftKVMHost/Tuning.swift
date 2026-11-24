import CoreMedia
import Foundation
import ScreenCaptureKit
import VideoToolbox

/// Capture/encode knobs, overridable from the environment for experiments.
/// Defaults are the shipping configuration.
enum Tuning {
    private static let env = ProcessInfo.processInfo.environment
    private static func int(_ k: String) -> Int? { env[k].flatMap { Int($0) } }

    /// SOFTKVM_CODEC=h264|hevc
    static let codec: Proto.Codec = env["SOFTKVM_CODEC"] == "h264" ? .h264 : .hevc
    /// SOFTKVM_NO_AUDIO=1 disables system audio capture.
    static let noAudio = env["SOFTKVM_NO_AUDIO"] == "1"
    /// SOFTKVM_MAX_INFLIGHT: frames allowed inside the encoder per display (0 = unlimited).
    static let maxInFlight = int("SOFTKVM_MAX_INFLIGHT") ?? 0

    /// SOFTKVM_ENC_LOWLAT=0 uses VideoToolbox's regular rate control instead of low-latency mode.
    static let encoderLowLatency = env["SOFTKVM_ENC_LOWLAT"] != "0"

    static func apply(to config: SCStreamConfiguration) {
        if let d = int("SOFTKVM_SCK_QUEUE_DEPTH") { config.queueDepth = d }
        if let fps = int("SOFTKVM_SCK_FPS") {
            config.minimumFrameInterval = fps == 0 ? .zero : CMTime(value: 1, timescale: CMTimeScale(fps))
        }
        if env["SOFTKVM_SCK_PIXFMT"] == "bgra" { config.pixelFormat = kCVPixelFormatType_32BGRA }
        if env["SOFTKVM_SCK_PIXFMT"] == "420f" { config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange }
    }

    static func apply(to session: VTCompressionSession) {
        if let n = int("SOFTKVM_ENC_MAX_DELAY") {
            VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxFrameDelayCount, value: n as CFNumber)
        }
    }
}
