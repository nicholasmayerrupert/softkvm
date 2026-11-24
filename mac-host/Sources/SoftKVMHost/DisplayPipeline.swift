import CGVirtualDisplayPrivate
import AVFoundation
import CoreMedia
import Foundation
import ScreenCaptureKit

/// A live virtual display that outlives a session (kept so windows don't get rearranged
/// when a PC reconnects). Releasing `object` removes the display.
struct VirtualDisplayHandle {
    let object: AnyObject
    let id: CGDirectDisplayID
    let request: Proto.DisplayRequest
}

/// One virtual display -> ScreenCaptureKit capture -> hardware encoder -> sink.
final class DisplayPipeline: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    typealias Sink = (_ annexB: Data, _ keyframe: Bool, _ ptsMicros: UInt64) -> Void
    /// Interleaved s16le stereo PCM at `Proto.audioSampleRate`.
    typealias AudioSink = (_ pcm: Data, _ ptsMicros: UInt64) -> Void

    let index: Int
    let request: Proto.DisplayRequest
    let codec: Proto.Codec = .hevc

    private let queue: DispatchQueue
    private let sink: Sink
    private let audioSink: AudioSink?
    private let audioQueue: DispatchQueue
    private var bitrate: Int

    private var virtualDisplay: AnyObject?
    private(set) var displayID: CGDirectDisplayID = 0
    private var stream: SCStream?
    private var encoder: VideoEncoder?

    // Idle refresh: ScreenCaptureKit stops delivering frames when nothing changes, so the
    // last (possibly blurry, fast-motion) frame would stay on screen forever. Re-encode the
    // last frame a few times after things settle so quality converges.
    private var lastBuffer: CVPixelBuffer?
    private var lastFrameTime: CFAbsoluteTime = 0
    private var refreshesLeft = 0
    private var refreshTimer: DispatchSourceTimer?
    private var keyframePending = true
    private var framesOut = 0
    private var bytesOut = 0
    private var statsStart = CFAbsoluteTimeGetCurrent()

    /// Pass `audioSink` on exactly one pipeline: system audio is global, not per display.
    private let reuse: VirtualDisplayHandle?

    init(index: Int, request: Proto.DisplayRequest, bitrate: Int, sink: @escaping Sink,
         audioSink: AudioSink? = nil, reuse: VirtualDisplayHandle? = nil) {
        self.reuse = reuse
        self.index = index
        self.request = request
        self.bitrate = bitrate
        self.sink = sink
        self.audioSink = audioSink
        self.queue = DispatchQueue(label: "softkvm.display.\(index)", qos: .userInteractive)
        self.audioQueue = DispatchQueue(label: "softkvm.audio.\(index)", qos: .userInteractive)
    }

    func start() async throws {
        let w = Int(request.width), h = Int(request.height), fps = Int(request.fps)

        let index = self.index
        let id: CGDirectDisplayID
        if let reuse {
            virtualDisplay = reuse.object
            id = reuse.id
            Log.info("display \(index): reusing virtual display id=\(id) \(w)x\(h)@\(fps)")
        } else {
            let (created, newID): (AnyObject?, CGDirectDisplayID) = await MainActor.run {
                var id: CGDirectDisplayID = 0
                let obj = SKCreateVirtualDisplay("SoftKVM \(index + 1)", UInt32(w), UInt32(h), Double(fps),
                                                 UInt32(index + 1), &id) as AnyObject?
                return (obj, id)
            }
            guard let created, newID != 0 else {
                throw NSError(domain: "SoftKVM", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "could not create virtual display"])
            }
            virtualDisplay = created
            id = newID
            Log.info("display \(index): created virtual display id=\(id) \(w)x\(h)@\(fps)")
        }
        displayID = id

        // The new display shows up in ScreenCaptureKit asynchronously.
        var scDisplay: SCDisplay?
        for _ in 0..<40 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            scDisplay = content.displays.first { $0.displayID == id }
            if scDisplay != nil { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        guard let scDisplay else {
            throw NSError(domain: "SoftKVM", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "virtual display never appeared in ScreenCaptureKit"])
        }

        encoder = try VideoEncoder(width: w, height: h, fps: fps, bitrate: bitrate, codec: codec) {
            [weak self] data, key, pts in
            self?.deliver(data, keyframe: key, pts: pts)
        }

        let config = SCStreamConfiguration()
        config.width = w
        config.height = h
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        config.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        config.colorSpaceName = CGColorSpace.sRGB
        config.showsCursor = false  // the client draws the cursor locally (no video latency)
        config.queueDepth = 5
        if audioSink != nil {
            config.capturesAudio = true
            config.sampleRate = Proto.audioSampleRate
            config.channelCount = 2
            config.excludesCurrentProcessAudio = true
        }

        let filter = SCContentFilter(display: scDisplay, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: config, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        if audioSink != nil {
            try stream.addStreamOutput(self, type: .audio, sampleHandlerQueue: audioQueue)
        }
        try await stream.startCapture()
        self.stream = stream
        Log.info("display \(index): capture started")

        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.1, repeating: 1.0 / 20.0)
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        refreshTimer = timer
    }

    /// Stops capture. With `keepDisplay`, returns the virtual display instead of removing it.
    @discardableResult
    func stop(keepDisplay: Bool = false) -> VirtualDisplayHandle? {
        refreshTimer?.cancel()
        refreshTimer = nil
        let stream = self.stream
        self.stream = nil
        Task { try? await stream?.stopCapture() }
        queue.sync {
            encoder?.invalidate()
            encoder = nil
            lastBuffer = nil
        }
        var kept: VirtualDisplayHandle?
        if keepDisplay, let obj = virtualDisplay, displayID != 0 {
            kept = VirtualDisplayHandle(object: obj, id: displayID, request: request)
        } else {
            // Releasing the object removes the virtual display.
            DispatchQueue.main.async { [virtualDisplay] in _ = virtualDisplay }
        }
        virtualDisplay = nil
        Log.info("display \(index): stopped\(kept != nil ? " (display kept)" : "")")
        return kept
    }

    func setBitrate(_ bitrate: Int) {
        queue.async {
            self.bitrate = bitrate
            self.encoder?.setBitrate(bitrate)
        }
    }

    func requestKeyframe() {
        queue.async {
            self.keyframePending = true
            // If the screen is idle no new frame will arrive; re-encode the last one now.
            if CFAbsoluteTimeGetCurrent() - self.lastFrameTime > 0.05 { self.encodeLast() }
        }
    }

    // MARK: - SCStreamOutput

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        if type == .audio {
            handleAudio(sb)
            return
        }
        guard type == .screen, sb.isValid,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false)
                as? [[SCStreamFrameInfo: Any]],
              let rawStatus = attachments.first?[.status] as? Int,
              SCFrameStatus(rawValue: rawStatus) == .complete,
              let pb = sb.imageBuffer else { return }
        lastBuffer = pb
        lastFrameTime = CFAbsoluteTimeGetCurrent()
        refreshesLeft = 8
        encode(pb)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        Log.info("display \(index): capture stopped with error: \(error.localizedDescription)")
    }

    // MARK: - Audio (on `audioQueue`)

    private var loggedAudioFormat = false

    private func handleAudio(_ sb: CMSampleBuffer) {
        guard let audioSink, sb.isValid, let fd = sb.formatDescription,
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd)?.pointee else { return }
        if !loggedAudioFormat {
            loggedAudioFormat = true
            Log.info("audio: \(asbd.mSampleRate) Hz, \(asbd.mChannelsPerFrame) ch, flags=\(asbd.mFormatFlags), "
                     + "bits=\(asbd.mBitsPerChannel)")
        }
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let nonInterleaved = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        guard isFloat, asbd.mBitsPerChannel == 32 else { return }
        let frames = CMSampleBufferGetNumSamples(sb)
        guard frames > 0 else { return }

        var pcm = Data(count: frames * 4)  // stereo s16
        do {
            try sb.withAudioBufferList { abl, _ in
                pcm.withUnsafeMutableBytes { raw in
                    let out = raw.bindMemory(to: Int16.self)
                    func s16(_ f: Float) -> Int16 { Int16(max(-1, min(1, f)) * 32767) }
                    let channels = Int(asbd.mChannelsPerFrame)
                    if nonInterleaved {
                        let l = abl[0].mData!.assumingMemoryBound(to: Float.self)
                        let r = (abl.count > 1 ? abl[1] : abl[0]).mData!.assumingMemoryBound(to: Float.self)
                        for i in 0..<frames {
                            out[2 * i] = s16(l[i])
                            out[2 * i + 1] = s16(r[i])
                        }
                    } else {
                        let p = abl[0].mData!.assumingMemoryBound(to: Float.self)
                        for i in 0..<frames {
                            out[2 * i] = s16(p[i * channels])
                            out[2 * i + 1] = s16(p[i * channels + (channels > 1 ? 1 : 0)])
                        }
                    }
                }
            }
        } catch {
            return
        }
        let micros = UInt64(max(0, CMTimeGetSeconds(sb.presentationTimeStamp)) * 1_000_000)
        audioSink(pcm, micros)
    }

    // MARK: - Internals (on `queue`)

    private func tick() {
        let idle = CFAbsoluteTimeGetCurrent() - lastFrameTime
        if refreshesLeft > 0, idle > 0.15 {
            refreshesLeft -= 1
            encodeLast()
        }
    }

    private func encodeLast() {
        guard let pb = lastBuffer else { return }
        encode(pb)
    }

    private func encode(_ pb: CVPixelBuffer) {
        let force = keyframePending
        keyframePending = false
        encoder?.encode(pb, pts: CMClockGetTime(CMClockGetHostTimeClock()), forceKeyframe: force)
    }

    private func deliver(_ data: Data, keyframe: Bool, pts: CMTime) {
        let micros = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000)
        sink(data, keyframe, micros)
        framesOut += 1
        bytesOut += data.count
        let now = CFAbsoluteTimeGetCurrent()
        if now - statsStart >= 5 {
            let secs = now - statsStart
            Log.info(String(format: "display %d: %.1f fps, %.1f Mbps", index,
                            Double(framesOut) / secs, Double(bytesOut) * 8 / secs / 1_000_000))
            framesOut = 0
            bytesOut = 0
            statsStart = now
        }
    }
}
