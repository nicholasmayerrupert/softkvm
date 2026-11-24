import AppKit
import CoreGraphics

/// Watches the system cursor so the client can draw it locally with zero video latency.
/// Reports shape changes (RGBA image + hotspot) and the cursor's global position.
final class CursorMonitor {
    /// payload for Proto CURSOR: i32 hotX, i32 hotY, u32 w, u32 h, w*h*4 RGBA (straight alpha)
    var onShape: ((Data) -> Void)?
    /// Global display coordinates (points, top-left origin).
    var onPosition: ((CGPoint) -> Void)?

    private var timer: Timer?
    private var lastPixels: Data?
    private var lastHotspot = CGPoint(x: -1, y: -1)

    func start() {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.timer?.invalidate()
            let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in self?.poll() }
            RunLoop.main.add(t, forMode: .common)
            self.timer = t
        }
    }

    func stop() {
        DispatchQueue.main.async {
            self.timer?.invalidate()
            self.timer = nil
        }
    }

    /// Forces the next poll to resend the shape (e.g. for a new client).
    func resendShape() {
        DispatchQueue.main.async { self.lastPixels = nil }
    }

    private func poll() {
        if let loc = CGEvent(source: nil)?.location { onPosition?(loc) }

        let cursor = NSCursor.currentSystem ?? NSCursor.arrow
        let image = cursor.image
        let w = Int(image.size.width.rounded()), h = Int(image.size.height.rounded())
        guard w > 0, h > 0, w <= 256, h <= 256 else { return }

        // Render at 1x: the streamed virtual displays are non-Retina.
        var pixels = Data(count: w * h * 4)
        let ok = pixels.withUnsafeMutableBytes { raw -> Bool in
            guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
            image.draw(in: NSRect(x: 0, y: 0, width: w, height: h))
            NSGraphicsContext.restoreGraphicsState()
            // Un-premultiply for the client.
            let p = raw.bindMemory(to: UInt8.self)
            for i in stride(from: 0, to: w * h * 4, by: 4) {
                let a = Int(p[i + 3])
                if a > 0 && a < 255 {
                    p[i] = UInt8(min(255, Int(p[i]) * 255 / a))
                    p[i + 1] = UInt8(min(255, Int(p[i + 1]) * 255 / a))
                    p[i + 2] = UInt8(min(255, Int(p[i + 2]) * 255 / a))
                }
            }
            return true
        }
        guard ok else { return }
        let hot = cursor.hotSpot
        if pixels == lastPixels && hot == lastHotspot { return }
        lastPixels = pixels
        lastHotspot = hot

        var payload = Data(capacity: 16 + pixels.count)
        payload.appendLE(Int32(hot.x.rounded()))
        payload.appendLE(Int32(hot.y.rounded()))
        payload.appendLE(UInt32(w))
        payload.appendLE(UInt32(h))
        payload.append(pixels)
        onShape?(payload)
    }
}

/// Current arrangement of every active Mac display, in global points.
enum DisplayLayout {
    struct Entry: Equatable {
        var stream: UInt8  // 0xFF = not streamed (e.g. the built-in screen)
        var rect: CGRect
    }

    static func current(streamDisplays: [CGDirectDisplayID]) -> [Entry] {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return [] }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetActiveDisplayList(count, &ids, &count) == .success else { return [] }
        return ids.prefix(Int(count)).compactMap { id in
            // Skip displays that mirror another one; they add no space for the cursor.
            if CGDisplayIsInMirrorSet(id) != 0 && CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay { return nil }
            let idx = streamDisplays.firstIndex(of: id)
            return Entry(stream: idx.map { UInt8($0) } ?? 0xFF, rect: CGDisplayBounds(id))
        }
    }

    static func payload(_ entries: [Entry]) -> Data {
        var d = Data()
        d.appendLE(UInt32(entries.count))
        for e in entries {
            d.append(e.stream)
            d.append(contentsOf: [0, 0, 0])
            d.appendLE(Int32(e.rect.minX.rounded()))
            d.appendLE(Int32(e.rect.minY.rounded()))
            d.appendLE(Int32(e.rect.width.rounded()))
            d.appendLE(Int32(e.rect.height.rounded()))
        }
        return d
    }

    /// Positions the streamed displays relative to each other the way the PC's monitors are
    /// arranged, keeping the first one where macOS put it (so its relation to the built-in
    /// screen stays whatever the user chose in System Settings > Displays).
    static func mirrorClientArrangement(displays: [CGDirectDisplayID], clientOrigins: [CGPoint]) {
        guard displays.count > 1, clientOrigins.count == displays.count else { return }
        let base = CGDisplayBounds(displays[0]).origin
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        for i in 1..<displays.count {
            let x = base.x + clientOrigins[i].x - clientOrigins[0].x
            let y = base.y + clientOrigins[i].y - clientOrigins[0].y
            CGConfigureDisplayOrigin(config, displays[i], Int32(x), Int32(y))
        }
        let err = CGCompleteDisplayConfiguration(config, .forSession)
        Log.info("layout: arranged streamed displays like the PC monitors (\(err == .success ? "ok" : "error \(err)"))")
    }
}
