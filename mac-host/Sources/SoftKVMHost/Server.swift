import CoreGraphics
import CryptoKit
import Foundation
import IOKit.pwr_mgt
import Network

/// TCP server. Every connection runs the encrypted handshake; only PCs the user has paired
/// (approved on this Mac) get in. One PC is in control at a time; others are told who has it
/// and may take over.
final class Server: @unchecked Sendable {
    let queue = DispatchQueue(label: "softkvm.server")
    private var listener: NWListener?
    private(set) var active: ClientSession?
    private var waiting: [ClientSession] = []  // handshaking, awaiting approval, or told "busy"
    private(set) var bitrate: Int
    let hostName: String

    /// Asks the user to approve a new PC (called on the main thread): name, pairing code, reply.
    var askPairing: ((String, String, @escaping (Bool) -> Void) -> Void)?

    // Virtual displays kept after an unexpected disconnect, so windows stay where they were.
    private var parked: [VirtualDisplayHandle] = []
    private var parkTimer: DispatchSourceTimer?
    private static let parkSeconds = 15 * 60

    private var sleepAssertion: IOPMAssertionID = 0

    init(bitrate: Int, hostName: String) {
        self.bitrate = bitrate
        self.hostName = hostName
    }

    func start() throws {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: HostEnv.port)!)
        listener.stateUpdateHandler = { state in Log.info("server: listener \(state)") }
        listener.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        listener.start(queue: queue)
        self.listener = listener
    }

    private static let maxWaiting = 8, maxWaitingPerAddress = 2
    private var promptOpen = false  // one pairing prompt at a time

    private func accept(_ conn: NWConnection) {
        Log.info("server: connection from \(conn.endpoint)")
        // Unauthenticated peers can't pile up connections (memory, prompts).
        let address = ClientSession.address(of: conn.endpoint)
        let fromThere = waiting.filter { $0.peerAddress == address }.count
        if waiting.count >= Self.maxWaiting || fromThere >= Self.maxWaitingPerAddress {
            Log.info("server: refusing \(address): too many pending connections")
            conn.cancel()
            return
        }
        let s = ClientSession(connection: conn, server: self)
        waiting.append(s)
        s.start()
    }

    // MARK: - Menu actions (any thread)

    func setBitrate(_ bitrate: Int) {
        queue.async {
            self.bitrate = bitrate
            self.active?.setBitrate(bitrate)
            Log.info("server: bitrate set to \(bitrate / 1_000_000) Mbps")
        }
    }

    /// Asks the PC to use these monitors (bit i = monitor index i). The PC reconnects.
    func requestMonitors(mask: UInt32) {
        queue.async { self.active?.requestMonitors(mask: mask) }
    }

    func disconnect() {
        queue.async { self.active?.sayGoodbye(.userDisconnected, "") }
    }

    /// The Mac is quitting or going to sleep: release input and tell every client why, so the PC
    /// shows the right state and reconnects later. Blocks briefly so the goodbye goes out.
    func shutdown(reason: String) {
        queue.sync {
            Log.info("server: \(reason); saying goodbye to connected PCs")
            if let a = active {
                active = nil
                a.sayGoodbye(.hostQuitting, "")
            }
            for s in waiting { s.sayGoodbye(.hostQuitting, "") }
            waiting.removeAll()
            setStayAwake(false)
        }
        Thread.sleep(forTimeInterval: 0.2)
    }

    /// Forgets a paired PC and disconnects it if it's here.
    func unpair(key: Data) {
        TrustStore.shared.remove(key: key)
        queue.async {
            if let a = self.active, a.clientKey == key { a.sayGoodbye(.unpaired, "") }
            for s in self.waiting where s.clientKey == key { s.sayGoodbye(.unpaired, "") }
            AppState.update { _ in }
        }
    }

    // MARK: - Session lifecycle (server queue)

    /// The handshake finished: decide whether this PC may connect.
    func authenticated(_ s: ClientSession) {
        if TrustStore.shared.isTrusted(s.clientKey) && !s.requestsPairing {
            admit(s, code: .ok)
            return
        }
        if promptOpen {
            Log.info("server: \(s.clientName) wants to pair, but another pairing is in progress")
            s.sendAuth(.pairingDenied, "another pairing is in progress")
            s.closeSoon("another pairing is in progress")
            return
        }
        Log.info("server: \(s.clientName) wants to pair (code \(s.pairingCode))")
        promptOpen = true
        s.sendAuth(.pairingPending, "")
        // Nobody answered in time: give up (this also closes the prompt).
        queue.asyncAfter(deadline: .now() + 120) {
            if s.state == .waitingForApproval {
                s.sendAuth(.pairingDenied, "not approved in time")
                s.closeSoon("approval timed out")
            }
        }
        let name = s.clientName, code = s.pairingCode
        DispatchQueue.main.async { [weak self] in
            guard let ask = self?.askPairing else { return }
            ask(name, code) { allowed in
                self?.queue.async {
                    self?.promptOpen = false
                    guard let self, !s.isClosed else { return }
                    if allowed {
                        TrustStore.shared.add(name: s.clientName, key: s.clientKey)
                        Log.info("server: paired with \(s.clientName)")
                        s.sendAuth(.paired, "")  // informational: the client remembers this Mac
                        self.admit(s, code: .ok)
                    } else {
                        Log.info("server: pairing with \(s.clientName) denied")
                        s.sendAuth(.pairingDenied, "")
                        s.closeSoon("pairing denied")
                    }
                }
            }
        }
    }

    private func admit(_ s: ClientSession, code: Proto.AuthCode) {
        if let a = active, a !== s {
            if a.clientKey == s.clientKey {
                // The same PC reconnecting (e.g. its old connection died silently): replace it.
                // Clear `active` first so closing it doesn't hand control to a waiting PC.
                active = nil
                a.close(reason: "replaced by the same PC reconnecting", keepDisplays: true)
            } else {
                Log.info("server: \(s.clientName) is waiting; \(a.clientName) is in control")
                s.state = .busy
                s.sendAuth(.busy, a.clientName)
                return
            }
        }
        makeActive(s, code: code)
    }

    private func makeActive(_ s: ClientSession, code: Proto.AuthCode) {
        waiting.removeAll { $0 === s }
        active = s
        s.state = .authorized
        s.sendAuth(code, "")
        Log.info("server: \(s.clientName) is in control")
        let name = s.clientName, address = s.peerAddress
        AppState.update { $0.clientName = name; $0.clientAddress = address }
        setStayAwake(true)
    }

    func takeover(by s: ClientSession) {
        guard s.state == .busy else { return }
        if let a = active {
            Log.info("server: \(s.clientName) takes over from \(a.clientName)")
            active = nil  // before revoking, so the old session's close doesn't promote anyone
            // A different PC: its displays don't carry over, so don't keep them around.
            a.sayGoodbye(.takenOver, s.clientName, keepDisplays: false)
        }
        makeActive(s, code: .ok)
    }

    func sessionClosed(_ s: ClientSession, displays: [VirtualDisplayHandle], keepDisplays: Bool) {
        waiting.removeAll { $0 === s }
        if active === s {
            active = nil
            AppState.update { $0.clientName = nil; $0.clientAddress = nil; $0.monitors = [] }
            setStayAwake(false)
            // Let the next PC that was told "busy" in.
            if let next = waiting.first(where: { $0.state == .busy }) { makeActive(next, code: .ok) }
        }
        if keepDisplays && !displays.isEmpty {
            park(displays)
        } else {
            release(displays)
        }
    }

    // MARK: - Kept displays

    /// Returns kept displays if they match what a new session asks for; otherwise removes them.
    func takeParked(matching requests: [Proto.DisplayRequest]) -> [VirtualDisplayHandle]? {
        defer { parked = []; parkTimer?.cancel(); parkTimer = nil }
        if !parked.isEmpty && parked.map(\.request) == requests {
            Log.info("server: reusing \(parked.count) kept display(s)")
            return parked
        }
        release(parked)
        return nil
    }

    private func park(_ displays: [VirtualDisplayHandle]) {
        release(parked)
        parked = displays
        Log.info("server: keeping \(displays.count) display(s) for \(Self.parkSeconds / 60) min")
        parkTimer?.cancel()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + .seconds(Self.parkSeconds))
        t.setEventHandler { [weak self] in
            guard let self else { return }
            Log.info("server: removing kept displays (timed out)")
            self.release(self.parked)
            self.parked = []
        }
        t.resume()
        parkTimer = t
    }

    private func release(_ displays: [VirtualDisplayHandle]) {
        guard !displays.isEmpty else { return }
        // Dropping the last reference removes the display; do it on the main thread.
        DispatchQueue.main.async { _ = displays }
    }

    // MARK: - Power

    /// Keeps the Mac from idle-sleeping while a PC is connected (it would drop the connection).
    private func setStayAwake(_ on: Bool) {
        if on, sleepAssertion == 0 {
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep as CFString,
                                        IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                        "SoftKVM: a PC is connected" as CFString, &sleepAssertion)
        } else if !on, sleepAssertion != 0 {
            IOPMAssertionRelease(sleepAssertion)
            sleepAssertion = 0
        }
    }
}

/// One connection: handshake, then encrypted messages.
final class ClientSession: @unchecked Sendable {
    enum State { case handshake, confirming, waitingForApproval, busy, authorized, streaming, closed }

    private let conn: NWConnection
    private let queue: DispatchQueue
    private weak var server: Server?
    var state = State.handshake
    var isClosed: Bool { state == .closed }

    private(set) var clientKey = Data()
    private(set) var clientName = "PC"
    private(set) var pairingCode = ""
    /// The client doesn't know this Mac yet and asked to see the pairing prompt even if this Mac
    /// already trusts it (so both screens show the code before it hands over control).
    private(set) var requestsPairing = false
    private var cipher: RecordCipher?

    private var pipelines: [DisplayPipeline] = []
    private let injector = InputInjector()
    private var heartbeat: DispatchSourceTimer?
    private let cursor = CursorMonitor()
    private var lastLayout: [DisplayLayout.Entry] = []
    private var lastSentCursorPos = CGPoint(x: -100_000, y: -100_000)
    private var streamsReady = false
    /// Last time anything arrived from the client. Authorized clients send at least one message a
    /// second (time sync), so silence means the link is dead: drop it before held keys linger.
    private var lastReceived = ProcessInfo.processInfo.systemUptime
    private static let silenceLimit: TimeInterval = 5

    // Backpressure: bytes handed to the socket but not yet sent.
    private let pendingLock = NSLock()
    private var pendingBytes = 0
    private var dropping = Set<Int>()           // streams waiting for a keyframe
    private var keyframeRequested = Set<Int>()  // ...and already asked for one
    private static let softLimit = 3 * 1024 * 1024
    private static let hardLimit = 12 * 1024 * 1024

    init(connection: NWConnection, server: Server) {
        self.conn = connection
        self.server = server
        self.queue = server.queue
    }

    var peerAddress: String { Self.address(of: conn.endpoint) }

    static func address(of endpoint: NWEndpoint) -> String {
        if case .hostPort(let host, _) = endpoint {
            switch host {
            case .ipv4(let a): return "\(a)"
            case .ipv6(let a): return "\(a)"
            case .name(let n, _): return n
            @unknown default: return "\(host)"
            }
        }
        return "\(endpoint)"
    }

    func start() {
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed(let err): self?.close(reason: "failed: \(err)", keepDisplays: true)
            case .cancelled: self?.close(reason: "cancelled", keepDisplays: true)
            default: break
            }
        }
        injector.displayForStream = { [weak self] i in
            guard let self, i < self.pipelines.count, self.pipelines[i].displayID != 0 else { return nil }
            let r = self.pipelines[i].request
            return (self.pipelines[i].displayID, CGSize(width: Int(r.width), height: Int(r.height)))
        }
        conn.start(queue: queue)
        readHandshake()
        // Unauthenticated connections get 10 s to finish the handshake.
        queue.asyncAfter(deadline: .now() + 10) { [weak self] in
            if self?.state == .handshake || self?.state == .confirming {
                self?.close(reason: "handshake timeout", keepDisplays: false)
            }
        }
    }

    // MARK: - Handshake

    private func readFramed(_ done: @escaping (Data) -> Void) {
        receiveExactly(4) { [weak self] lenData in
            guard let self, let len = lenData.readLE(UInt32.self, at: 0), len > 0,
                  len <= UInt32(Handshake.maxMessage) else {
                self?.close(reason: "not a SoftKVM client", keepDisplays: false)
                return
            }
            self.receiveExactly(Int(len)) { body in done(body) }
        }
    }

    private func sendFramed(_ body: Data, then: ((NWError?) -> Void)? = nil) {
        var framed = Data()
        framed.appendLE(UInt32(body.count))
        framed.append(body)
        conn.send(content: framed, completion: .contentProcessed { error in then?(error) })
    }

    private var appVersion: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev" }

    private func hostHello(status: Handshake.Status, ephemeral: Curve25519.KeyAgreement.PrivateKey) -> Data {
        Handshake.encode(.init(
            proto: Handshake.protocolVersion, status: status.rawValue, staticKey: Identity.publicKey,
            ephemeralKey: ephemeral.publicKey.rawRepresentation, name: server?.hostName ?? "Mac",
            version: appVersion, platform: Handshake.Platform.mac.rawValue))
    }

    /// v2 handshake: commitment → HostHello → ClientHello (checked against the commitment) →
    /// keys → the client's first record (CONFIRM) proves it holds its private key.
    private func readHandshake() {
        readFramed { [weak self] first in
            guard let self else { return }
            if let commit = Handshake.decodeCommit(first), commit.proto == Handshake.protocolVersion {
                self.handleCommit(commit.hash)
                return
            }
            // Anything else is another protocol version (a v1 client sends its hello directly).
            let theirs = Handshake.decodeCommit(first)?.proto ?? Handshake.decode(first)?.proto
            let name = Handshake.decode(first)?.name ?? "a PC"
            let theirVersion = Handshake.decode(first)?.version ?? "?"
            Log.info("session: \(name) uses protocol v\(theirs.map(String.init) ?? "?") (app \(theirVersion)); "
                     + "this Mac speaks v\(Handshake.protocolVersion) (app \(self.appVersion))")
            self.sendFramed(self.hostHello(status: .versionMismatch, ephemeral: Curve25519.KeyAgreement.PrivateKey())) {
                [weak self] _ in self?.queue.async { self?.close(reason: "version mismatch", keepDisplays: false) }
            }
        }
    }

    private func handleCommit(_ commitment: Data) {
        let ephemeral = Curve25519.KeyAgreement.PrivateKey()
        let hostHello = self.hostHello(status: .ok, ephemeral: ephemeral)
        sendFramed(hostHello)
        readFramed { [weak self] body in
            guard let self else { return }
            // The client chose its hello before seeing ours, so neither side can grind for a
            // pairing code that matches another connection's.
            guard Data(SHA512.hash(data: body)) == commitment else {
                return self.close(reason: "client hello doesn't match its commitment", keepDisplays: false)
            }
            guard let hello = Handshake.decode(body), hello.proto == Handshake.protocolVersion else {
                return self.close(reason: "malformed client hello", keepDisplays: false)
            }
            self.clientName = Handshake.displayName(hello.name)
            do {
                let keys = try Handshake.hostKeys(clientHello: body, hostHello: hostHello, client: hello,
                                                  hostEphemeral: ephemeral)
                self.cipher = RecordCipher(keys: keys)
                self.pairingCode = keys.pairingCode
                self.clientKey = hello.staticKey
            } catch {
                return self.close(reason: "key agreement failed", keepDisplays: false)
            }
            self.state = .confirming
            Log.info("session: handshake with \(self.clientName) (\(deviceID(self.clientKey)), app \(hello.version)); "
                     + "waiting for key confirmation")
            self.readRecord()
        }
    }

    private func receiveExactly(_ n: Int, _ done: @escaping (Data) -> Void) {
        conn.receive(minimumIncompleteLength: n, maximumLength: n) { [weak self] data, _, isComplete, error in
            guard let self, self.state != .closed else { return }
            if let error { return self.close(reason: "read error \(error)", keepDisplays: true) }
            guard let data, data.count == n else {
                return self.close(reason: isComplete ? "client disconnected" : "short read", keepDisplays: true)
            }
            done(data)
        }
    }

    // MARK: - Encrypted records

    private func readRecord() {
        receiveExactly(4) { [weak self] header in
            guard let self else { return }
            let len = Int(header.readLE(UInt32.self, at: 0) ?? 0)
            // Everything a PC sends is small (input, monitor lists); don't buffer megabytes from a
            // peer before its record has even been authenticated.
            guard len >= 16 + Proto.headerSize, len <= 64 * 1024 + Proto.headerSize + 16 else {
                return self.close(reason: "bad record length", keepDisplays: true)
            }
            self.receiveExactly(len) { body in
                guard let plain = self.cipher?.open(header: header, body: body) else {
                    return self.close(reason: "record failed authentication", keepDisplays: true)
                }
                self.lastReceived = ProcessInfo.processInfo.systemUptime
                self.handleMessage(plain)
                if self.state != .closed { self.readRecord() }
            }
        }
    }

    private func handleMessage(_ m: Data) {
        guard m.count >= Proto.headerSize else { return }
        let type = m[m.startIndex], stream = m[m.startIndex + 1]
        let len = Int(m.readLE(UInt32.self, at: 4) ?? 0)
        guard Proto.headerSize + len <= m.count else { return }
        let payload = m.subdata(in: (m.startIndex + Proto.headerSize)..<(m.startIndex + Proto.headerSize + len))
        let msg = Proto.MsgType(rawValue: type)

        if state == .confirming {
            // Key confirmation: the first record must be CONFIRM. Only a client holding the
            // private key it presented can produce it, so nobody can pose as a paired PC.
            guard msg == .confirm else { return close(reason: "no key confirmation", keepDisplays: false) }
            requestsPairing = (payload.first ?? 0) & 1 != 0
            state = .waitingForApproval
            Log.info("session: \(clientName) confirmed its key\(requestsPairing ? "; it asks to pair" : "")")
            startHeartbeat()
            server?.authenticated(self)
            return
        }

        switch msg {
        case .goodbye:
            let switching = payload.first == 1
            close(reason: switching ? "the PC switched to another device" : "the PC quit",
                  keepDisplays: switching)
            return
        case .takeover:
            server?.takeover(by: self)
            return
        default:
            break
        }
        // Everything else needs an approved session that is the one in control.
        guard state == .authorized || state == .streaming, server?.active === self else { return }
        if let reply = TimingProto.handle(type: type, payload: payload) { return send(reply) }
        switch msg {
        case .hello:
            handleHello(payload)
        case .keyframeRequest:
            if Int(stream) < pipelines.count {
                Log.info("session: keyframe requested for stream \(stream)")
                pipelines[Int(stream)].requestKeyframe()
            }
        case .input:
            if !HostEnv.perf { injector.handle(payload) }
        case .monitors:
            handleMonitors(payload)
        default:
            Log.info("session: ignoring message type \(type)")
        }
    }

    // MARK: - Control

    func sendAuth(_ code: Proto.AuthCode, _ text: String) {
        send(Proto.message(.authStatus, payload: Proto.stringPayload(code.rawValue, text)))
    }

    func setBitrate(_ b: Int) {
        pipelines.forEach { $0.setBitrate(b) }
    }

    func requestMonitors(mask: UInt32) {
        var d = Data()
        d.appendLE(mask)
        send(Proto.message(.setMonitors, payload: d))
        Log.info("session: asked the PC to use monitor mask \(mask)")
    }

    /// Tells the client this is deliberate (so it doesn't reconnect by itself), then closes.
    /// Tells the client this is deliberate (so it doesn't reconnect by itself) and revokes the
    /// session right away: input is released and streams stop now; the socket closes once the
    /// goodbye has gone out.
    func sayGoodbye(_ reason: Proto.ByeReason, _ text: String, keepDisplays: Bool = false) {
        guard state != .closed else { return }
        send(Proto.message(.bye, payload: Proto.stringPayload(reason.rawValue, text)))
        close(reason: "goodbye (\(reason))", keepDisplays: keepDisplays, flushFirst: true)
    }

    /// Revokes now; closes the socket once queued data has gone out.
    func closeSoon(_ reason: String, keepDisplays: Bool = false) {
        close(reason: reason, keepDisplays: keepDisplays, flushFirst: true)
    }

    /// Tears the session down synchronously (input released, streams stopped, server told).
    /// With `flushFirst`, the socket is closed after already-queued data is sent (at most 2 s).
    func close(reason: String, keepDisplays: Bool, flushFirst: Bool = false) {
        guard state != .closed else { return }
        if state == .waitingForApproval, !pairingCode.isEmpty {
            let code = pairingCode
            PairingPrompt.cancel(code: code)  // nobody left to approve
        }
        state = .closed
        Log.info("session: \(clientName) closed (\(reason))")
        injector.releaseAll()
        cursor.stop()
        heartbeat?.cancel()
        heartbeat = nil
        if flushFirst {
            // Strong captures: the connection must close even if this session is gone by then.
            let conn = self.conn
            conn.send(content: nil, contentContext: .finalMessage, isComplete: true,
                      completion: .contentProcessed { _ in conn.cancel() })
            queue.asyncAfter(deadline: .now() + 2) { conn.cancel() }
        } else {
            conn.cancel()
        }
        if HostEnv.perf {
            // Close the test windows before their displays go away, so macOS never moves
            // them onto a real screen.
            let ps = pipelines
            pipelines = []
            DispatchQueue.main.async {
                PerfDisplays.stopAnimating()
                DispatchQueue.global().async { ps.forEach { $0.stop() } }
            }
            server?.sessionClosed(self, displays: [], keepDisplays: false)
            return
        }
        let handles = pipelines.compactMap { $0.stop(keepDisplay: keepDisplays) }
        pipelines = []
        server?.sessionClosed(self, displays: handles, keepDisplays: keepDisplays)
    }

    private func startHeartbeat() {
        // Lets the client detect a dead link even when the screen is idle.
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            if (self.state == .authorized || self.state == .streaming),
               ProcessInfo.processInfo.systemUptime - self.lastReceived > Self.silenceLimit {
                self.close(reason: "the PC went silent", keepDisplays: true)
                return
            }
            self.send(Proto.message(.ping, payload: TimingProto.pingPayload()))
            self.sendLayoutIfChanged()
        }
        timer.resume()
        heartbeat = timer
    }

    // MARK: - Streams

    private func handleHello(_ p: Data) {
        guard pipelines.isEmpty, let server else { return }
        guard let version = p.readLE(UInt32.self, at: 0), let n = p.readLE(UInt32.self, at: 4),
              n >= 1, n <= 4 else { return close(reason: "bad hello", keepDisplays: false) }
        var requests: [Proto.DisplayRequest] = []
        for i in 0..<Int(n) {
            let o = 8 + i * 12
            guard let w = p.readLE(UInt32.self, at: o), let h = p.readLE(UInt32.self, at: o + 4),
                  let f = p.readLE(UInt32.self, at: o + 8),
                  (320...8192).contains(w), (240...8192).contains(h), (1...240).contains(f)
            else { return close(reason: "bad display request", keepDisplays: false) }
            requests.append(.init(width: w, height: h, fps: f))
        }
        Log.info("session: hello v\(version), displays: \(requests.map { "\($0.width)x\($0.height)@\($0.fps)" })")
        state = .streaming

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
        let nameBytes = Array(server.hostName.utf8.prefix(255))
        ack.append(UInt8(nameBytes.count))
        ack.append(contentsOf: nameBytes)
        send(Proto.message(.helloAck, payload: ack))

        let reuse = server.takeParked(matching: requests)
        for (i, r) in requests.enumerated() {
            let pipeline = DisplayPipeline(
                index: i, request: r, bitrate: server.bitrate,
                sink: { [weak self] data, key, pts, timing in
                    self?.sendVideo(stream: i, data: data, keyframe: key, pts: pts, timing: timing)
                },
                audioSink: i == 0 ? { [weak self] pcm, pts in self?.sendAudio(pcm: pcm, pts: pts) } : nil,
                reuse: reuse?[i])
            pipeline.onCaptureFailed = { [weak self] in
                self?.queue.async { self?.close(reason: "screen capture stopped", keepDisplays: false) }
            }
            pipelines.append(pipeline)

            var info = Data()
            info.appendLE(r.width)
            info.appendLE(r.height)
            info.appendLE(r.fps)
            info.appendLE(pipeline.codec.rawValue)
            send(Proto.message(.streamInfo, stream: UInt8(i), payload: info))
        }

        let ps = pipelines
        let reused = reuse != nil
        Task {
            for p in ps {
                do { try await p.start() } catch {
                    Log.info("session: display \(p.index) failed to start: \(error.localizedDescription)")
                    self.queue.async { self.close(reason: "display start failed", keepDisplays: false) }
                    return
                }
            }
            if HostEnv.perf {
                PerfDisplays.tuckAway(ps.map(\.displayID))
                if HostEnv.animate { await PerfDisplays.animate(ps.map(\.displayID)) }
            } else if !reused {  // kept displays are already where the user left them
                DisplayLayout.mirrorClientArrangement(displays: ps.map(\.displayID), clientOrigins: clientOrigins)
            }
            self.queue.async { self.streamsReady = true; self.startCursor() }
        }
    }

    /// MONITORS: u8 nameLen, name, u8 n, then n × {u8 index, u8 selected, u32 w, u32 h, u32 hz, u8 len, name}
    private func handleMonitors(_ p: Data) {
        var o = 0
        func u8() -> UInt8? { defer { o += 1 }; return p.readLE(UInt8.self, at: o) }
        func u32() -> UInt32? { defer { o += 4 }; return p.readLE(UInt32.self, at: o) }
        func str() -> String? {
            guard let len = u8(), o + Int(len) <= p.count else { return nil }
            let s = String(decoding: p[(p.startIndex + o)..<(p.startIndex + o + Int(len))], as: UTF8.self)
            o += Int(len)
            return s
        }
        guard let pcName = str(), let n = u8() else { return }
        var list: [PCMonitor] = []
        for _ in 0..<n {
            guard let idx = u8(), let sel = u8(), let w = u32(), let h = u32(), let hz = u32(), let name = str()
            else { return }
            list.append(PCMonitor(index: Int(idx), name: name, width: Int(w), height: Int(h), refresh: Int(hz),
                                  selected: sel != 0))
        }
        Log.info("session: PC \"\(pcName)\" has \(list.count) monitors: "
                 + list.map { "\($0.name)\($0.selected ? "*" : "")" }.joined(separator: ", "))
        let shownName = Handshake.displayName(pcName)
        let shownList = list.map { m -> PCMonitor in var m = m; m.name = Handshake.displayName(m.name); return m }
        AppState.update { $0.clientName = shownName; $0.monitors = shownList }
    }

    // MARK: - Cursor and layout

    private func startCursor() {
        guard state != .closed else { return }
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
        guard state == .streaming else { return }
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
        guard streamsReady, state == .streaming else { return }
        let layout = DisplayLayout.current(streamDisplays: pipelines.map(\.displayID))
        guard layout != lastLayout, !layout.isEmpty else { return }
        lastLayout = layout
        Log.info("layout: " + layout.map { "\($0.stream == 0xFF ? "mac" : "s\($0.stream)")@\(Int($0.rect.minX)),\(Int($0.rect.minY)) \(Int($0.rect.width))x\(Int($0.rect.height))" }.joined(separator: "  "))
        send(Proto.message(.layout, payload: DisplayLayout.payload(layout)))
    }

    // MARK: - Sending

    private func send(_ plain: Data) {
        guard let cipher, state != .closed else { return }
        cipher.seal(plain) { record in
            pendingLock.lock(); pendingBytes += record.count; pendingLock.unlock()
            let size = record.count
            conn.send(content: record, completion: .contentProcessed { [weak self] error in
                guard let self else { return }
                self.pendingLock.lock()
                self.pendingBytes -= size
                // Backlog drained: streams that dropped frames need a fresh keyframe to recover.
                var recover: [Int] = []
                if self.pendingBytes < Self.softLimit / 2, !self.dropping.isEmpty {
                    recover = self.dropping.filter { !self.keyframeRequested.contains($0) }
                    self.keyframeRequested.formUnion(recover)
                }
                self.pendingLock.unlock()
                if let error { self.queue.async { self.close(reason: "send error \(error)", keepDisplays: true) } }
                if !recover.isEmpty {
                    Log.info("session: backlog drained, requesting keyframes for \(recover)")
                    self.queue.async {
                        for s in recover where s < self.pipelines.count { self.pipelines[s].requestKeyframe() }
                    }
                }
            })
        }
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

    private func sendVideo(stream: Int, data: Data, keyframe: Bool, pts: UInt64, timing: FrameTiming) {
        pendingLock.lock()
        let pending = pendingBytes
        var drop = false
        if keyframe {
            drop = pending > Self.hardLimit
            if drop {
                dropping.insert(stream)
                keyframeRequested.remove(stream)  // ask again once drained
                Log.info("session: stream \(stream) keyframe dropped (\(pending / 1024) KB unsent)")
            } else {
                if dropping.contains(stream) { Log.info("session: stream \(stream) recovered with \(data.count / 1024) KB keyframe") }
                dropping.remove(stream)
                keyframeRequested.remove(stream)
            }
        } else {
            // Once a P-frame is dropped, everything until the next keyframe is useless.
            drop = dropping.contains(stream) || pending > Self.softLimit
            if drop, !dropping.contains(stream) {
                dropping.insert(stream)
                Log.info("session: stream \(stream) backlogged (\(pending / 1024) KB unsent), dropping until keyframe")
            }
        }
        pendingLock.unlock()
        if drop { return }  // a keyframe is requested once the backlog drains (see send)

        send(TimingProto.frameTimingMessage(stream: stream, pts: pts, timing: timing, pendingBytes: pending))
        var payload = Data(capacity: 9 + data.count)
        payload.appendLE(pts)
        payload.append(keyframe ? 1 : 0)
        payload.append(data)
        send(Proto.message(.video, stream: UInt8(stream), payload: payload))
    }
}
