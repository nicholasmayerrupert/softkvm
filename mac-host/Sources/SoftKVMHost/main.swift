import AppKit
import ScreenCaptureKit

/// Menu-bar-only app: listens for a SoftKVM client and streams virtual displays to it.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillTerminate(_ notification: Notification) {
        server?.shutdown(reason: "quitting")
    }

    private var statusMenu: StatusMenu?
    private var server: Server?
    private var discovery: DiscoveryResponder?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let bitrateMbps = Int(ProcessInfo.processInfo.environment["SOFTKVM_BITRATE_MBPS"] ?? "")
            ?? AppState.shared.bitrateMbps
        let hostName = Host.current().localizedName ?? ProcessInfo.processInfo.hostName

        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
        Log.info("SoftKVM host \(version) starting (protocol v\(Handshake.protocolVersion), "
                 + "device \(deviceID(Identity.publicKey)), bitrate \(bitrateMbps) Mbps)")

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
        server.askPairing = { name, code, reply in PairingPrompt.ask(name: name, code: code, reply: reply) }
        // Going to sleep: let the PC know now (and release its keys) instead of it finding out
        // from a dead connection. It reconnects after wake.
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.willSleepNotification, object: nil, queue: nil
        ) { [weak server] _ in server?.shutdown(reason: "Mac going to sleep") }
        self.server = server
        if !Proto.headless && !HostEnv.perf {
            statusMenu = StatusMenu(server: server)
            let discovery = DiscoveryResponder(name: { hostName })
            discovery.start()
            self.discovery = discovery
        } else {
            Log.info("test instance on port \(Proto.port): no menu, not discoverable")
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
