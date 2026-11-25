import Darwin
import Foundation
import Security
import XPC

/// The launch daemon runs the same signed executable, in a mode with no UI or network listener.
enum AWDLHelper {
    static let name = "dev.softkvm.host.awdl"
    private static let recovery = URL(fileURLWithPath: "/var/run/\(name).restore")

    static func run() -> Never {
        guard geteuid() == 0, let requirement = signingRequirement() else { exit(EXIT_FAILURE) }
        let queue = DispatchQueue.main
        let listener = xpc_connection_create_mach_service(name, queue, UInt64(XPC_CONNECTION_MACH_SERVICE_LISTENER))
        guard xpc_connection_set_peer_code_signing_requirement(listener, requirement) == 0 else {
            exit(EXIT_FAILURE)
        }

        let interface = Interface()
        interface.update()  // recover an unfinished pause before accepting new connections

        signal(SIGTERM, SIG_IGN)
        let termination = DispatchSource.makeSignalSource(signal: SIGTERM, queue: queue)
        termination.setEventHandler {
            interface.viewers.removeAll()
            interface.update()
            exit(EXIT_SUCCESS)
        }
        termination.resume()

        xpc_connection_set_event_handler(listener) { peer in
            guard xpc_get_type(peer) == XPC_TYPE_CONNECTION else { return }
            xpc_connection_set_target_queue(peer, queue)
            let id = ObjectIdentifier(peer)
            xpc_connection_set_event_handler(peer) { event in
                if xpc_get_type(event) == XPC_TYPE_DICTIONARY, xpc_dictionary_get_bool(event, "pause") {
                    if interface.viewers.insert(id).inserted { interface.update() }
                } else if xpc_get_type(event) == XPC_TYPE_ERROR {
                    if interface.viewers.remove(id) != nil { interface.update() }
                }
            }
            xpc_connection_resume(peer)
        }
        xpc_connection_resume(listener)
        withExtendedLifetime((listener, termination)) { dispatchMain() }
    }

    // The client must be this exact build, including for locally/ad-hoc signed builds.
    private static func signingRequirement() -> String? {
        var code: SecCode?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code else { return nil }
        var staticCode: SecStaticCode?
        guard SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess, let staticCode else { return nil }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(staticCode, SecCSFlags(rawValue: kSecCSSigningInformation),
                                            &info) == errSecSuccess,
              let hash = (info as? [String: Any])?[kSecCodeInfoUnique as String] as? Data,
              hash.count == 20 else { return nil }
        return "cdhash H\"\(hash.map { String(format: "%02x", $0) }.joined())\""
    }

    private final class Interface {
        var viewers = Set<ObjectIdentifier>()
        private var paused = false
        private var restoreUp = FileManager.default.fileExists(atPath: recovery.path)
        private let timer = DispatchSource.makeTimerSource(queue: .main)
        private var monitoring = false

        init() {
            timer.schedule(deadline: .distantFuture)
            timer.setEventHandler { [weak self] in self?.update() }
            timer.resume()
        }

        func update() {
            defer { refreshTimer() }
            if viewers.isEmpty {
                paused = false
                if restoreUp, setUp(true) {
                    do {
                        try FileManager.default.removeItem(at: recovery)
                        restoreUp = false
                    } catch {
                        // Retain the marker and retry on the next tick.
                    }
                }
            } else if let up = isUp() {
                if !paused {
                    // Record the original state before changing it so launchd can recover after a crash.
                    if up && !restoreUp {
                        do { try Data().write(to: recovery, options: .atomic) }
                        catch { return }
                        restoreUp = true
                    }
                    paused = true
                }
                if up { _ = setUp(false) }
            }
        }

        private func refreshTimer() {
            let needed = !viewers.isEmpty || restoreUp
            guard needed != monitoring else { return }
            monitoring = needed
            if needed {
                timer.schedule(deadline: .now() + .milliseconds(250), repeating: .milliseconds(250),
                               leeway: .milliseconds(30))
            } else {
                timer.schedule(deadline: .distantFuture)
            }
        }

        private func isUp() -> Bool? {
            var addresses: UnsafeMutablePointer<ifaddrs>?
            guard getifaddrs(&addresses) == 0 else { return nil }
            defer { freeifaddrs(addresses) }
            return sequence(first: addresses, next: { $0?.pointee.ifa_next })
                .compactMap { $0 }
                .first { String(cString: $0.pointee.ifa_name) == "awdl0" }
                .map { ($0.pointee.ifa_flags & UInt32(IFF_UP)) != 0 }
        }

        private func setUp(_ up: Bool) -> Bool {
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/sbin/ifconfig")
            task.arguments = ["awdl0", up ? "up" : "down"]
            task.standardOutput = FileHandle.nullDevice
            task.standardError = FileHandle.nullDevice
            do { try task.run() } catch { return false }
            task.waitUntilExit()
            return task.terminationStatus == 0
        }
    }
}
