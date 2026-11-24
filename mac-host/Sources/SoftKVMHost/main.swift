import AppKit
import ScreenCaptureKit

/// Menu-bar-only app: listens for a SoftKVM client and streams virtual displays to it.
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusMenu: StatusMenu?
    private var server: Server?
    private var discovery: DiscoveryResponder?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let bitrateMbps = Int(ProcessInfo.processInfo.environment["SOFTKVM_BITRATE_MBPS"] ?? "")
            ?? AppState.shared.bitrateMbps
        let hostName = Host.current().localizedName ?? ProcessInfo.processInfo.hostName

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

        // Accessibility lets us inject the client's keyboard and mouse. Prompts on first run.
        Log.info("accessibility permission: \(InputInjector.checkPermission(prompt: true) ? "OK" : "missing")")

        let server = Server(bitrate: bitrateMbps * 1_000_000, hostName: hostName)
        do {
            try server.start()
        } catch {
            Log.info("failed to start server: \(error)")
            NSApp.terminate(nil)
        }
        self.server = server
        guard !HostEnv.perf else { return }  // benchmark instances: no menu, not discoverable
        statusMenu = StatusMenu(server: server)
        let discovery = DiscoveryResponder(name: { hostName })
        discovery.start()
        self.discovery = discovery
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
