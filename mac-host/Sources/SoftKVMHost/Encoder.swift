import CoreMedia
import Foundation
import VideoToolbox

/// Hardware HEVC/H.264 encoder producing Annex B access units.
final class VideoEncoder {
    typealias Output = (_ annexB: Data, _ keyframe: Bool, _ pts: CMTime, _ timing: FrameTiming) -> Void

    let codec: Proto.Codec
    private var session: VTCompressionSession?
    private let output: Output

    init(width: Int, height: Int, fps: Int, bitrate: Int, codec: Proto.Codec, output: @escaping Output) throws {
        self.codec = codec
        self.output = output

        let codecType = codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
        var session: VTCompressionSession?
        var lowLatency = Tuning.encoderLowLatency
        var status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height), codecType: codecType,
            encoderSpecification: Self.spec(lowLatency: Tuning.encoderLowLatency), imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        if status != noErr {
            Log.info("encoder: low-latency mode unavailable (\(status)), falling back")
            lowLatency = false
            status = VTCompressionSessionCreate(
                allocator: nil, width: Int32(width), height: Int32(height), codecType: codecType,
                encoderSpecification: Self.spec(lowLatency: false), imageBufferAttributes: nil,
                compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        }
        guard status == noErr, let session else {
            throw NSError(domain: "SoftKVM.Encoder", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "VTCompressionSessionCreate failed: \(status)"])
        }
        self.session = session

        func set(_ key: CFString, _ value: CFTypeRef) {
            let s = VTSessionSetProperty(session, key: key, value: value)
            if s != noErr { Log.info("encoder: property \(key) not supported (\(s))") }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel,
            codec == .hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
        // Long GOP: desktops are mostly static; clients ask for keyframes when they need one.
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (fps * 120) as CFNumber)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 120 as CFNumber)
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        if !lowLatency {
            // Cap bursts at ~1.5x average over one second.
            set(kVTCompressionPropertyKey_DataRateLimits, [bitrate * 3 / 2 / 8, 1] as CFArray)
        }
        Tuning.apply(to: session)
        VTCompressionSessionPrepareToEncodeFrames(session)
        Log.info("encoder: \(codec) \(width)x\(height)@\(fps) \(bitrate / 1_000_000) Mbps lowLatency=\(lowLatency)")
    }

    private static func spec(lowLatency: Bool) -> CFDictionary {
        var d: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
        if lowLatency { d[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
        return d as CFDictionary
    }

    /// `completion` runs once per call when VideoToolbox is done with the frame (encoded,
    /// dropped or failed), on a VideoToolbox thread, after `output`.
    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, forceKeyframe: Bool, timing: FrameTiming = FrameTiming(),
                completion: (() -> Void)? = nil) {
        guard let session else { completion?(); return }
        let props: CFDictionary? = forceKeyframe
            ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, _, sampleBuffer in
            defer { completion?() }
            var timing = timing
            timing.encodedUs = HostClock.nowMicros()
            guard let self, status == noErr, let sampleBuffer else {
                if status != noErr { Log.info("encoder: frame error \(status)") }
                return
            }
            self.emit(sampleBuffer, timing: timing)
        }
        if status != noErr {
            Log.info("encoder: encode call failed \(status)")
            completion?()
        }
    }

    func invalidate() {
        if let session {
            VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(session)
        }
        session = nil
    }

    deinit { invalidate() }

    // MARK: - Annex B conversion

    private static let startCode: [UInt8] = [0, 0, 0, 1]

    private func emit(_ sb: CMSampleBuffer, timing: FrameTiming) {
        let keyframe = Self.isKeyframe(sb)
        var out = Data()
        if keyframe, let fd = sb.formatDescription {
            appendParameterSets(fd, to: &out)
        }
        guard let block = sb.dataBuffer else { return }
        var totalLength = 0
        var ptr: UnsafeMutablePointer<CChar>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil,
                                          totalLengthOut: &totalLength, dataPointerOut: &ptr) == noErr,
              let ptr else { return }
        // VideoToolbox emits AVCC/HVCC: 4-byte big-endian NAL lengths.
        let raw = UnsafeRawPointer(ptr)
        var offset = 0
        while offset + 4 <= totalLength {
            let len = Int(raw.load(fromByteOffset: offset, as: UInt32.self).bigEndian)
            offset += 4
            guard len > 0, offset + len <= totalLength else { break }
            out.append(contentsOf: Self.startCode)
            out.append(raw.advanced(by: offset).assumingMemoryBound(to: UInt8.self), count: len)
            offset += len
        }
        output(out, keyframe, sb.presentationTimeStamp, timing)
    }

    private func appendParameterSets(_ fd: CMFormatDescription, to out: inout Data) {
        var count = 0
        let getter: (Int, UnsafeMutablePointer<UnsafePointer<UInt8>?>, UnsafeMutablePointer<Int>,
                     UnsafeMutablePointer<Int>?) -> OSStatus = { index, p, size, countOut in
            if self.codec == .hevc {
                return CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fd, parameterSetIndex: index, parameterSetPointerOut: p, parameterSetSizeOut: size,
                    parameterSetCountOut: countOut, nalUnitHeaderLengthOut: nil)
            } else {
                return CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    fd, parameterSetIndex: index, parameterSetPointerOut: p, parameterSetSizeOut: size,
                    parameterSetCountOut: countOut, nalUnitHeaderLengthOut: nil)
            }
        }
        var p: UnsafePointer<UInt8>?
        var size = 0
        guard getter(0, &p, &size, &count) == noErr else { return }
        for i in 0..<count {
            guard getter(i, &p, &size, nil) == noErr, let p else { continue }
            out.append(contentsOf: Self.startCode)
            out.append(p, count: size)
        }
    }

    private static func isKeyframe(_ sb: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false)
                as? [[CFString: Any]], let first = attachments.first else { return true }
        return !(first[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)
    }
}
