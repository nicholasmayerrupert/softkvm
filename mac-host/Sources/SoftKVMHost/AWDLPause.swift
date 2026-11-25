import Foundation
import ServiceManagement
import XPC

/// Lives on the server queue. A connection is the lease to pause AWDL.
final class AWDLPause {
    private let queue: DispatchQueue
    private var viewing = false
    private var connection: xpc_connection_t?
    private var retry: DispatchSourceTimer?

    static let service = SMAppService.daemon(plistName: "\(AWDLHelper.name).plist")

    static var enabled: Bool {
        get { UserDefaults.standard.bool(forKey: "pauseAWDLWhileStreaming") }
        set { UserDefaults.standard.set(newValue, forKey: "pauseAWDLWhileStreaming") }
    }

    init(queue: DispatchQueue) { self.queue = queue }

    func setViewing(_ viewing: Bool) {
        self.viewing = viewing
        refresh()
    }

    func refresh() {
        guard viewing && Self.enabled else {
            retry?.cancel()
            retry = nil
            if let connection { xpc_connection_cancel(connection) }
            connection = nil
            return
        }
        if retry == nil {
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + 1, repeating: 1)
            timer.setEventHandler { [weak self] in self?.connectIfApproved() }
            retry = timer
            timer.resume()
        }
        connectIfApproved()
    }

    private func connectIfApproved() {
        guard connection == nil, Self.service.status == .enabled else { return }
        let peer = xpc_connection_create_mach_service(AWDLHelper.name, queue,
                                                       UInt64(XPC_CONNECTION_MACH_SERVICE_PRIVILEGED))
        xpc_connection_set_event_handler(peer) { [weak self, weak peer] event in
            guard let self, let peer, self.connection === peer, xpc_get_type(event) == XPC_TYPE_ERROR else { return }
            xpc_connection_cancel(peer)
            self.connection = nil
        }
        connection = peer
        xpc_connection_resume(peer)
        let request = xpc_dictionary_create(nil, nil, 0)
        xpc_dictionary_set_bool(request, "pause", true)
        xpc_connection_send_message(peer, request)
    }
}
