import AppKit
import ScreenCaptureKit

/// Menu-bar-only app: listens for a SoftKVM client and streams virtual displays to it.
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private var server: Server?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let bitrateMbps = Int(ProcessInfo.processInfo.environment["SOFTKVM_BITRATE_MBPS"] ?? "") ?? 40

        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "KVM"
        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "SoftKVM host — port \(Proto.port)", action: nil, keyEquivalent: ""))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        item.menu = menu
        statusItem = item

        Log.info("SoftKVM host starting (bitrate \(bitrateMbps) Mbps)")

        // Trigger the Screen Recording permission prompt up front rather than on first connect.
        Task {
            do {
                _ = try await SCShareableContent.current
                Log.info("screen recording permission OK")
            } catch {
                Log.info("screen recording permission missing: \(error.localizedDescription)")
            }
        }

        let server = Server(bitrate: bitrateMbps * 1_000_000)
        do {
            try server.start()
        } catch {
            Log.info("failed to start server: \(error)")
            NSApp.terminate(nil)
        }
        self.server = server
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
