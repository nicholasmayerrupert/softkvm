import Darwin
import Foundation

/// A monitor on the connected PC, as reported by the client.
struct PCMonitor: Equatable {
    var index: Int
    var name: String
    var width: Int
    var height: Int
    var refresh: Int
    var selected: Bool
}

/// UI-facing state. Only touched on the main thread.
final class AppState {
    static let shared = AppState()

    var clientName: String?      // nil while no PC is connected
    var clientAddress: String?
    var monitors: [PCMonitor] = []
    var onChange: (() -> Void)?

    static let videoQualities: [(name: String, mbps: Int)] = [
        ("Standard", 20), ("High", 40), ("Maximum", 80),
    ]

    var bitrateMbps: Int {
        get {
            let v = UserDefaults.standard.integer(forKey: "bitrateMbps")
            return v > 0 ? v : 40
        }
        set { UserDefaults.standard.set(newValue, forKey: "bitrateMbps") }
    }

    /// Posts a change from any thread.
    static func update(_ change: @escaping (AppState) -> Void) {
        DispatchQueue.main.async {
            change(shared)
            shared.onChange?()
        }
    }

    /// This Mac's IPv4 addresses on real interfaces (Wi-Fi, Ethernet), for display.
    static func localAddresses() -> [String] {
        var result: [String] = []
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return [] }
        defer { freeifaddrs(ifaddr) }
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = ptr.pointee
            guard let sa = ifa.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET),
                  (ifa.ifa_flags & UInt32(IFF_UP)) != 0, (ifa.ifa_flags & UInt32(IFF_LOOPBACK)) == 0 else { continue }
            let name = String(cString: ifa.ifa_name)
            guard name.hasPrefix("en") else { continue }  // skip VPN/tunnel interfaces
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                result.append(String(cString: host))
            }
        }
        return result
    }
}
