import CoreGraphics
import Foundation
import Network

/// TCP server. Accepts one client at a time; a new client replaces the old one.
final class Server: @unchecked Sendable {
    private let queue = DispatchQueue(label: "softkvm.server")
    private var listener: NWListener?
    private var session: ClientSession?
    let bitrate: Int

    init(bitrate: Int) {
        self.bitrate = bitrate
    }

    func start() throws {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Proto.port)!)
        listener.stateUpdateHandler = { state in Log.info("server: listener \(state)") }
        listener.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        listener.start(queue: queue)
        self.listener = listener
    }

    private func accept(_ conn: NWConnection) {
        Log.info("server: connection from \(conn.endpoint)")
        session?.close(reason: "replaced by new client")
        let s = ClientSession(connection: conn, bitrate: bitrate, queue: queue)
        s.onClose = { [weak self, weak s] in
            if let self, self.session === s { self.session = nil }
        }
        session = s
        s.start()
    }
}

/// One connected client and the display pipelines it asked for.
final class ClientSession: @unchecked Sendable {
    private let conn: NWConnection
    private let queue: DispatchQueue
    private let bitrate: Int
    private var pipelines: [DisplayPipeline] = []
    private let injector = InputInjector()
    private var closed = false
    private var heartbeat: DispatchSourceTimer?
    private let cursor = CursorMonitor()
    private var lastLayout: [DisplayLayout.Entry] = []
    private var lastSentCursorPos = CGPoint(x: -100_000, y: -100_000)
    private var streamsReady = false
    var onClose: (() -> Void)?

    // Backpressure: bytes handed to the socket but not yet sent.
    private let pendingLock = NSLock()
    private var pendingBytes = 0
    private var dropping = Set<Int>()           // streams waiting for a keyframe
    private var keyframeRequested = Set<Int>()  // ...and already asked for one
    private static let softLimit = 3 * 1024 * 1024
    private static let hardLimit = 12 * 1024 * 1024

    init(connection: NWConnection, bitrate: Int, queue: DispatchQueue) {
        self.conn = connection
        self.bitrate = bitrate
        self.queue = queue
    }

    func start() {
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed(let err): self?.close(reason: "failed: \(err)")
            case .cancelled: self?.close(reason: "cancelled")
            default: break
            }
        }
        injector.displayForStream = { [weak self] i in
            guard let self, i < self.pipelines.count, self.pipelines[i].displayID != 0 else { return nil }
            let r = self.pipelines[i].request
            return (self.pipelines[i].displayID, CGSize(width: Int(r.width), height: Int(r.height)))
        }
        conn.start(queue: queue)
        readHeader()

        // Heartbeat so the client can detect a dead link even when the screen is idle.
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            self?.send(Proto.message(.ping))
            self?.sendLayoutIfChanged()
        }
        timer.resume()
        heartbeat = timer
    }

    func close(reason: String) {
        guard !closed else { return }
        closed = true
        Log.info("session: closed (\(reason))")
        injector.releaseAll()
        cursor.stop()
        heartbeat?.cancel()
        heartbeat = nil
        conn.cancel()
        let ps = pipelines
        pipelines = []
        DispatchQueue.global().async { ps.forEach { $0.stop() } }
        onClose?()
    }

    // MARK: - Receiving

    private func readHeader() {
        conn.receive(minimumIncompleteLength: Proto.headerSize, maximumLength: Proto.headerSize) {
            [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let error { return self.close(reason: "read error \(error)") }
            guard let data, data.count == Proto.headerSize else {
                return self.close(reason: isComplete ? "client disconnected" : "short header")
            }
            let type = data[data.startIndex]
            let stream = data[data.startIndex + 1]
            let len = Int(data.readLE(UInt32.self, at: 4) ?? 0)
            guard len <= Proto.maxPayload else { return self.close(reason: "payload too large") }
            if len == 0 {
                self.handle(type: type, stream: stream, payload: Data())
                self.readHeader()
            } else {
                self.conn.receive(minimumIncompleteLength: len, maximumLength: len) {
                    [weak self] payload, _, _, error in
                    guard let self else { return }
                    if let error { return self.close(reason: "read error \(error)") }
                    guard let payload, payload.count == len else { return self.close(reason: "short payload") }
                    self.handle(type: type, stream: stream, payload: payload)
                    self.readHeader()
                }
            }
        }
    }

    private func handle(type: UInt8, stream: UInt8, payload: Data) {
        switch Proto.MsgType(rawValue: type) {
        case .hello:
            handleHello(payload)
        case .keyframeRequest:
            if Int(stream) < pipelines.count {
                Log.info("session: keyframe requested for stream \(stream)")
                pipelines[Int(stream)].requestKeyframe()
            }
        case .input:
            injector.handle(payload)
        default:
            Log.info("session: ignoring message type \(type)")
        }
    }

    private func handleHello(_ p: Data) {
        guard pipelines.isEmpty else { return }
        guard let version = p.readLE(UInt32.self, at: 0), let n = p.readLE(UInt32.self, at: 4),
              n >= 1, n <= 4 else { return close(reason: "bad hello") }
        var requests: [Proto.DisplayRequest] = []
        for i in 0..<Int(n) {
            let o = 8 + i * 12
            guard let w = p.readLE(UInt32.self, at: o), let h = p.readLE(UInt32.self, at: o + 4),
                  let f = p.readLE(UInt32.self, at: o + 8),
                  (320...8192).contains(w), (240...8192).contains(h), (1...240).contains(f)
            else { return close(reason: "bad display request") }
            requests.append(.init(width: w, height: h, fps: f))
        }
        Log.info("session: hello v\(version), displays: \(requests.map { "\($0.width)x\($0.height)@\($0.fps)" })")

        // Optional extension: each PC monitor's desktop origin, so we can arrange alike.
        var clientOrigins: [CGPoint] = []
        let extBase = 8 + Int(n) * 12
        if p.count >= extBase + Int(n) * 8 {
            for i in 0..<Int(n) {
                let x = p.readLE(Int32.self, at: extBase + i * 8) ?? 0
                let y = p.readLE(Int32.self, at: extBase + i * 8 + 4) ?? 0
                clientOrigins.append(CGPoint(x: Int(x), y: Int(y)))
            }
        }

        var ack = Data()
        ack.appendLE(Proto.version)
        send(Proto.message(.helloAck, payload: ack))

        for (i, r) in requests.enumerated() {
            let pipeline = DisplayPipeline(
                index: i, request: r, bitrate: bitrate,
                sink: { [weak self] data, key, pts in
                    self?.sendVideo(stream: i, data: data, keyframe: key, pts: pts)
                },
                audioSink: i == 0 ? { [weak self] pcm, pts in self?.sendAudio(pcm: pcm, pts: pts) } : nil)
            pipelines.append(pipeline)

            var info = Data()
            info.appendLE(r.width)
            info.appendLE(r.height)
            info.appendLE(r.fps)
            info.appendLE(pipeline.codec.rawValue)
            send(Proto.message(.streamInfo, stream: UInt8(i), payload: info))
        }

        let ps = pipelines
        Task {
            for p in ps {
                do { try await p.start() } catch {
                    Log.info("session: display \(p.index) failed to start: \(error.localizedDescription)")
                    self.queue.async { self.close(reason: "display start failed") }
                    return
                }
            }
            DisplayLayout.mirrorClientArrangement(displays: ps.map(\.displayID), clientOrigins: clientOrigins)
            self.queue.async { self.streamsReady = true; self.startCursor() }
        }
    }

    // MARK: - Cursor and layout

    private func startCursor() {
        guard !closed else { return }
        sendLayoutIfChanged()
        cursor.onShape = { [weak self] payload in
            self?.queue.async { self?.send(Proto.message(.cursor, payload: payload)) }
        }
        cursor.onPosition = { [weak self] pos in
            self?.queue.async { self?.cursorMoved(to: pos) }
        }
        cursor.resendShape()
        cursor.start()
    }

    /// Tells the client where the cursor is when something other than the client moved it
    /// (the MacBook trackpad, an app warping it), so the local cursor stays in sync.
    private func cursorMoved(to pos: CGPoint) {
        guard !closed else { return }
        let sinceOurMove = ProcessInfo.processInfo.systemUptime - injector.lastMoveTime
        guard sinceOurMove > 0.3 else { lastSentCursorPos = pos; return }
        guard abs(pos.x - lastSentCursorPos.x) > 0.5 || abs(pos.y - lastSentCursorPos.y) > 0.5 else { return }
        lastSentCursorPos = pos
        injector.syncPosition(pos)
        var d = Data()
        d.appendLE(Float(pos.x).bitPattern)
        d.appendLE(Float(pos.y).bitPattern)
        send(Proto.message(.cursorPos, payload: d))
    }

    private func sendLayoutIfChanged() {
        guard streamsReady, !closed else { return }
        let layout = DisplayLayout.current(streamDisplays: pipelines.map(\.displayID))
        guard layout != lastLayout, !layout.isEmpty else { return }
        lastLayout = layout
        Log.info("layout: " + layout.map { "\($0.stream == 0xFF ? "mac" : "s\($0.stream)")@\(Int($0.rect.minX)),\(Int($0.rect.minY)) \(Int($0.rect.width))x\(Int($0.rect.height))" }.joined(separator: "  "))
        send(Proto.message(.layout, payload: DisplayLayout.payload(layout)))
    }

    // MARK: - Sending

    private func send(_ data: Data) {
        pendingLock.lock(); pendingBytes += data.count; pendingLock.unlock()
        conn.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let self else { return }
            self.pendingLock.lock()
            self.pendingBytes -= data.count
            // Backlog drained: streams that dropped frames need a fresh keyframe to recover.
            var recover: [Int] = []
            if self.pendingBytes < Self.softLimit / 2, !self.dropping.isEmpty {
                recover = self.dropping.filter { !self.keyframeRequested.contains($0) }
                self.keyframeRequested.formUnion(recover)
            }
            self.pendingLock.unlock()
            if let error { self.queue.async { self.close(reason: "send error \(error)") } }
            if !recover.isEmpty {
                self.queue.async {
                    for s in recover where s < self.pipelines.count { self.pipelines[s].requestKeyframe() }
                }
            }
        })
    }

    private func sendAudio(pcm: Data, pts: UInt64) {
        // Audio is tiny; only drop it if the link is badly backed up.
        pendingLock.lock()
        let pending = pendingBytes
        pendingLock.unlock()
        if pending > Self.hardLimit { return }
        var payload = Data(capacity: 8 + pcm.count)
        payload.appendLE(pts)
        payload.append(pcm)
        send(Proto.message(.audio, payload: payload))
    }

    private func sendVideo(stream: Int, data: Data, keyframe: Bool, pts: UInt64) {
        pendingLock.lock()
        let pending = pendingBytes
        var drop = false
        if keyframe {
            drop = pending > Self.hardLimit
            if drop {
                dropping.insert(stream)
                keyframeRequested.remove(stream)  // ask again once drained
            } else {
                dropping.remove(stream)
                keyframeRequested.remove(stream)
            }
        } else {
            // Once a P-frame is dropped, everything until the next keyframe is useless.
            drop = dropping.contains(stream) || pending > Self.softLimit
            if drop, !dropping.contains(stream) { dropping.insert(stream) }
        }
        pendingLock.unlock()

        if drop {
            // A keyframe is requested once the backlog drains (see send completion).
            return
        }

        var payload = Data(capacity: 9 + data.count)
        payload.appendLE(pts)
        payload.append(keyframe ? 1 : 0)
        payload.append(data)
        send(Proto.message(.video, stream: UInt8(stream), payload: payload))
    }
}
