import AppKit
import ApplicationServices
import CoreGraphics
import Foundation

/// Replays keyboard/mouse events from the client into macOS via CGEvent.
/// Requires Accessibility permission (prompted at startup).
///
/// INPUT payload (all little-endian), first byte = kind:
///   1 MOUSE_MOVE   u8 display, i32 x, i32 y      (pixels within that display's stream)
///   2 MOUSE_BUTTON u8 button (0 L, 1 R, 2 M, 3 back, 4 forward), u8 down
///   3 SCROLL       f32 dx, f32 dy                 (wheel notches; dy > 0 = scroll up)
///   4 KEY          u16 macKeyCode, u8 down, u8 repeat
///   5 RELEASE_ALL
///   6 MOUSE_MOVE_GLOBAL f32 x, f32 y            (global Mac display points)
final class InputInjector {
    enum Kind: UInt8 {
        case mouseMove = 1, mouseButton = 2, scroll = 3, key = 4, releaseAll = 5, mouseMoveGlobal = 6
    }

    /// When we last moved the cursor, so the session can tell our moves from the user's
    /// (e.g. someone touching the MacBook trackpad).
    private(set) var lastMoveTime: TimeInterval = 0

    /// Adopts a cursor position that changed outside our control.
    func syncPosition(_ p: CGPoint) { position = p }

    /// Maps a stream index to (displayID, stream pixel size).
    var displayForStream: (Int) -> (CGDirectDisplayID, CGSize)? = { _ in nil }

    private let source = CGEventSource(stateID: .hidSystemState)
    private var position = CGPoint.zero
    private var buttonsDown = Set<Int>()
    private var keysDown = Set<UInt16>()
    private var capsLock = false
    private var lastClick: (button: Int, time: TimeInterval, pos: CGPoint, count: Int)?
    private var scrollRemainder = CGPoint.zero

    /// Pixels scrolled per wheel notch. Override with SOFTKVM_SCROLL_PX.
    private let pixelsPerNotch: Double =
        Double(ProcessInfo.processInfo.environment["SOFTKVM_SCROLL_PX"] ?? "") ?? 60

    static func checkPermission(prompt: Bool) -> Bool {
        let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: prompt] as CFDictionary
        return AXIsProcessTrustedWithOptions(opts)
    }

    func handle(_ p: Data) {
        guard let raw = p.first, let kind = Kind(rawValue: raw) else { return }
        switch kind {
        case .mouseMove:
            guard let d = p.readLE(UInt8.self, at: 1), let x = p.readLE(Int32.self, at: 2),
                  let y = p.readLE(Int32.self, at: 6) else { return }
            mouseMove(stream: Int(d), x: Double(x), y: Double(y))
        case .mouseButton:
            guard let b = p.readLE(UInt8.self, at: 1), let down = p.readLE(UInt8.self, at: 2) else { return }
            mouseButton(Int(b), down: down != 0)
        case .scroll:
            guard let dx = p.readLE(UInt32.self, at: 1), let dy = p.readLE(UInt32.self, at: 5) else { return }
            scroll(dx: Double(Float(bitPattern: dx)), dy: Double(Float(bitPattern: dy)))
        case .key:
            guard let code = p.readLE(UInt16.self, at: 1), let down = p.readLE(UInt8.self, at: 3) else { return }
            let isRepeat = (p.readLE(UInt8.self, at: 4) ?? 0) != 0
            key(code, down: down != 0, isRepeat: isRepeat)
        case .releaseAll:
            releaseAll()
        case .mouseMoveGlobal:
            guard let x = p.readLE(UInt32.self, at: 1), let y = p.readLE(UInt32.self, at: 5) else { return }
            moveTo(CGPoint(x: Double(Float(bitPattern: x)), y: Double(Float(bitPattern: y))))
        }
    }

    /// Lifts every key and button we pressed, so nothing stays stuck when the client
    /// switches away or disconnects.
    func releaseAll() {
        for code in keysDown { key(code, down: false, isRepeat: false) }
        for b in buttonsDown { mouseButton(b, down: false) }
        keysDown.removeAll()
        buttonsDown.removeAll()
    }

    // MARK: - Mouse

    private func mouseMove(stream: Int, x: Double, y: Double) {
        guard let (display, size) = displayForStream(stream) else { return }
        let bounds = CGDisplayBounds(display)  // global points
        guard bounds.width > 0, size.width > 0 else { return }
        let sx = bounds.width / size.width, sy = bounds.height / size.height
        let pt = CGPoint(x: bounds.minX + min(max(x, 0), size.width - 1) * sx,
                         y: bounds.minY + min(max(y, 0), size.height - 1) * sy)
        moveTo(pt)
    }

    private func moveTo(_ pt: CGPoint) {
        lastMoveTime = ProcessInfo.processInfo.systemUptime
        let dx = pt.x - position.x, dy = pt.y - position.y
        position = pt

        let (type, button): (CGEventType, CGMouseButton)
        if buttonsDown.contains(0) { (type, button) = (.leftMouseDragged, .left) }
        else if buttonsDown.contains(1) { (type, button) = (.rightMouseDragged, .right) }
        else if let other = buttonsDown.first { (type, button) = (.otherMouseDragged, CGMouseButton(rawValue: UInt32(other))!) }
        else { (type, button) = (.mouseMoved, .left) }

        guard let ev = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: pt,
                               mouseButton: button) else { return }
        ev.setIntegerValueField(.mouseEventDeltaX, value: Int64(dx.rounded()))
        ev.setIntegerValueField(.mouseEventDeltaY, value: Int64(dy.rounded()))
        ev.flags = currentFlags()
        ev.post(tap: .cghidEventTap)
    }

    private func mouseButton(_ b: Int, down: Bool) {
        guard (0...4).contains(b) else { return }
        if down { buttonsDown.insert(b) } else if buttonsDown.remove(b) == nil { return }

        let type: CGEventType
        let button: CGMouseButton
        switch b {
        case 0: type = down ? .leftMouseDown : .leftMouseUp; button = .left
        case 1: type = down ? .rightMouseDown : .rightMouseUp; button = .right
        default: type = down ? .otherMouseDown : .otherMouseUp; button = CGMouseButton(rawValue: UInt32(b))!
        }

        // macOS needs an explicit click count for double/triple clicks.
        var count = 1
        if down {
            let now = ProcessInfo.processInfo.systemUptime
            if let last = lastClick, last.button == b, now - last.time < NSEvent.doubleClickInterval,
               abs(last.pos.x - position.x) < 5, abs(last.pos.y - position.y) < 5 {
                count = last.count + 1
            }
            lastClick = (b, now, position, count)
        } else if let last = lastClick, last.button == b {
            count = last.count
        }

        guard let ev = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: position,
                               mouseButton: button) else { return }
        ev.setIntegerValueField(.mouseEventClickState, value: Int64(count))
        if b >= 2 { ev.setIntegerValueField(.mouseEventButtonNumber, value: Int64(b)) }
        ev.flags = currentFlags()
        ev.post(tap: .cghidEventTap)
    }

    private func scroll(dx: Double, dy: Double) {
        scrollRemainder.x += dx * pixelsPerNotch
        scrollRemainder.y += dy * pixelsPerNotch
        let px = Int32(scrollRemainder.x.rounded(.towardZero))
        let py = Int32(scrollRemainder.y.rounded(.towardZero))
        guard px != 0 || py != 0 else { return }
        scrollRemainder.x -= Double(px)
        scrollRemainder.y -= Double(py)
        // wheel1 = vertical (positive = up), wheel2 = horizontal (positive = left).
        guard let ev = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2,
                               wheel1: py, wheel2: -px, wheel3: 0) else { return }
        ev.flags = currentFlags()
        ev.post(tap: .cghidEventTap)
    }

    // MARK: - Keyboard

    private static let modifierFlags: [UInt16: CGEventFlags] = [
        0x37: .maskCommand, 0x36: .maskCommand,
        0x38: .maskShift, 0x3C: .maskShift,
        0x3A: .maskAlternate, 0x3D: .maskAlternate,
        0x3B: .maskControl, 0x3E: .maskControl,
        0x3F: .maskSecondaryFn,
    ]
    private static let capsLockKey: UInt16 = 0x39

    private func currentFlags() -> CGEventFlags {
        var f: CGEventFlags = []
        for k in keysDown { if let m = Self.modifierFlags[k] { f.insert(m) } }
        if capsLock { f.insert(.maskAlphaShift) }
        return f
    }

    private func key(_ code: UInt16, down: Bool, isRepeat: Bool) {
        if down {
            keysDown.insert(code)
        } else if keysDown.remove(code) == nil {
            return  // key-up for a key we never pressed (e.g. held while switching to the Mac)
        }

        if code == Self.capsLockKey {
            if down { capsLock.toggle() } else { return }
        }

        guard let ev = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
        if Self.modifierFlags[code] != nil || code == Self.capsLockKey {
            ev.type = .flagsChanged
        }
        if isRepeat { ev.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        ev.flags = currentFlags()
        ev.post(tap: .cghidEventTap)
    }
}
