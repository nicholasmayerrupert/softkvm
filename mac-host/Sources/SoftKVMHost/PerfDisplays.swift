import AppKit
import CoreGraphics
import QuartzCore

/// Helpers for benchmark instances (SOFTKVM_PERF=1): keep the test displays out of the way of
/// the real ones, and give them something to stream.
enum PerfDisplays {
    /// Moves the given displays so they only touch the rest of the arrangement at its
    /// bottom-right corner, where the cursor is unlikely to wander onto them. Uses an
    /// app-only configuration, so macOS restores the arrangement when this process exits.
    static func tuckAway(_ ours: [CGDirectDisplayID]) {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetActiveDisplayList(count, &ids, &count) == .success else { return }
        var maxX = CGFloat(-1e9), maxY = CGFloat(-1e9)
        for id in ids.prefix(Int(count)) where !ours.contains(id) {
            let b = CGDisplayBounds(id)
            maxX = max(maxX, b.maxX)
            maxY = max(maxY, b.maxY)
        }
        guard maxX > -1e9 else { return }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        var x = maxX
        for id in ours {
            CGConfigureDisplayOrigin(config, id, Int32(x), Int32(maxY))
            x += CGDisplayBounds(id).width
        }
        let err = CGCompleteDisplayConfiguration(config, .forAppOnly)
        Log.info("perf: tucked test displays at \(Int(maxX)),\(Int(maxY)) (\(err == .success ? "ok" : "error \(err)")): "
                 + ours.map { "\(CGDisplayBounds($0))" }.joined(separator: " "))
    }

    private static var windows: [CGDirectDisplayID: NSWindow] = [:]
    private static var observer: NSObjectProtocol?

    private static func number(_ s: NSScreen) -> CGDirectDisplayID? {
        (s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
    }

    /// AppKit frame (bottom-left origin) -> CoreGraphics global rect (top-left origin).
    private static func cgRect(_ f: NSRect) -> CGRect {
        let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
        return CGRect(x: f.minX, y: primaryHeight - f.maxY, width: f.width, height: f.height)
    }

    /// True if `win` is entirely on display `id` and nowhere else.
    private static func placedCorrectly(_ win: NSWindow, on id: CGDirectDisplayID) -> Bool {
        guard let s = win.screen, number(s) == id else { return false }
        return cgRect(win.frame) == CGDisplayBounds(id) && cgRect(s.frame) == CGDisplayBounds(id)
    }

    /// Covers each display with a window that scrolls a page of text (like scrolling a
    /// document) plus a spinning square, animated by Core Animation at the display's rate.
    /// A window is only made visible after verifying it sits exactly on its test display,
    /// and is closed if the display configuration changes underneath it.
    @MainActor
    static func animate(_ ids: [CGDirectDisplayID]) async {
        for id in ids {
            // Wait until AppKit's view of the display matches CoreGraphics' (after tuckAway).
            var screen: NSScreen?
            for _ in 0..<50 {
                if let s = NSScreen.screens.first(where: { number($0) == id }), cgRect(s.frame) == CGDisplayBounds(id) {
                    screen = s
                    break
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            guard let screen else { Log.info("perf: display \(id) never settled; not animating it"); continue }
            let frame = screen.frame
            let win = NSWindow(contentRect: NSRect(origin: .zero, size: frame.size), styleMask: [.borderless],
                               backing: .buffered, defer: false, screen: screen)
            win.alphaValue = 0  // invisible until its placement is verified
            win.setFrame(frame, display: false)
            win.ignoresMouseEvents = true
            win.isReleasedWhenClosed = false
            win.backgroundColor = .white
            win.collectionBehavior = [.stationary, .ignoresCycle]
            win.orderFrontRegardless()
            guard placedCorrectly(win, on: id) else {
                Log.info("perf: window for display \(id) landed at \(cgRect(win.frame)), expected "
                         + "\(CGDisplayBounds(id)); closing it")
                win.close()
                continue
            }
            let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
            view.wantsLayer = true
            win.contentView = view
            if let root = view.layer { addAnimation(to: root, size: frame.size) }
            win.alphaValue = 1
            windows[id] = win
            Log.info("perf: animating display \(id) at \(cgRect(win.frame))")
        }
        if observer == nil {
            observer = NotificationCenter.default.addObserver(
                forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    for (id, win) in windows where !placedCorrectly(win, on: id) {
                        Log.info("perf: display \(id) changed; closing its test window")
                        win.close()
                        windows[id] = nil
                    }
                }
            }
        }
    }

    /// Closes all test windows (session end).
    @MainActor
    static func stopAnimating() {
        for win in windows.values { win.close() }
        windows.removeAll()
    }

    private static func addAnimation(to root: CALayer, size: CGSize) {
        // Two screens' worth of text, scrolled continuously (SOFTKVM_ANIMATE_SPEED points/s).
        let speed = Double(ProcessInfo.processInfo.environment["SOFTKVM_ANIMATE_SPEED"] ?? "") ?? 600
        let page = CALayer()
        page.frame = CGRect(x: 0, y: 0, width: size.width, height: size.height * 2)
        page.contents = textImage(width: Int(size.width), height: Int(size.height * 2))
        page.contentsGravity = .resize
        root.addSublayer(page)
        let scroll = CABasicAnimation(keyPath: "position.y")
        scroll.fromValue = size.height
        scroll.toValue = 0
        scroll.duration = size.height / speed
        scroll.repeatCount = .infinity
        page.add(scroll, forKey: "scroll")

        let box = CALayer()
        box.frame = CGRect(x: size.width / 2 - 150, y: size.height / 2 - 150, width: 300, height: 300)
        box.backgroundColor = NSColor.systemOrange.cgColor
        box.cornerRadius = 24
        root.addSublayer(box)
        let spin = CABasicAnimation(keyPath: "transform.rotation.z")
        spin.fromValue = 0
        spin.toValue = Double.pi * 2
        spin.duration = 2
        spin.repeatCount = .infinity
        box.add(spin, forKey: "spin")
    }

    private static func textImage(width: Int, height: Int) -> CGImage? {
        guard let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
        let words = ["latency", "frame", "pacing", "encoder", "display", "packet", "texture", "present",
                     "keyboard", "stream", "virtual", "capture", "decoder", "socket", "vsync", "swap"]
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 15),
                                                    .foregroundColor: NSColor.black]
        var y = height - 24, line = 0
        while y > 0 {
            var s = "\(line)  "
            for i in 0..<28 { s += words[(line * 7 + i * 3) % words.count] + " " }
            (s as NSString).draw(at: NSPoint(x: 24, y: y), withAttributes: attrs)
            y -= 22
            line += 1
        }
        NSGraphicsContext.restoreGraphicsState()
        return ctx.makeImage()
    }
}
