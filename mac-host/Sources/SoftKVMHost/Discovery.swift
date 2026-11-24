import Darwin
import Foundation

/// Answers LAN discovery broadcasts so PCs can find this Mac without typing an address.
/// Request: "SOFTKVM_DISCOVER". Reply: "SOFTKVM_HOST" + u16 LE TCP port + UTF-8 name.
final class DiscoveryResponder {
    private var fd: Int32 = -1
    private let queue = DispatchQueue(label: "softkvm.discovery")
    private var source: DispatchSourceRead?
    private let name: () -> String

    init(name: @escaping () -> String) {
        self.name = name
    }

    func start() {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return Log.info("discovery: socket failed") }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Proto.discoveryPort.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else {
            Log.info("discovery: bind to UDP \(Proto.discoveryPort) failed (\(errno))")
            close(fd)
            fd = -1
            return
        }
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        src.setEventHandler { [weak self] in self?.receive() }
        src.resume()
        source = src
        Log.info("discovery: listening on UDP \(Proto.discoveryPort)")
    }

    private func receive() {
        var buf = [UInt8](repeating: 0, count: 256)
        var from = sockaddr_in()
        var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        let n = withUnsafeMutablePointer(to: &from) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                recvfrom(fd, &buf, buf.count, 0, $0, &fromLen)
            }
        }
        guard n >= Proto.discoverRequest.count,
              Array(buf[0..<Proto.discoverRequest.count]) == Proto.discoverRequest else { return }

        var reply = Data(Proto.discoverReply)
        reply.appendLE(HostEnv.port)
        reply.append(Data(name().utf8.prefix(64)))
        _ = reply.withUnsafeBytes { bytes in
            withUnsafePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, bytes.baseAddress, bytes.count, 0, $0, fromLen)
                }
            }
        }
    }
}
