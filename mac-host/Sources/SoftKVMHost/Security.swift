import CryptoKit
import Foundation

/// Long-term X25519 identity of this Mac, stored in Application Support (0600).
enum Identity {
    static let shared: Curve25519.KeyAgreement.PrivateKey = load()

    static var publicKey: Data { shared.publicKey.rawRepresentation }

    static var dir: URL {
        let d = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SoftKVM", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true,
                                                 attributes: [.posixPermissions: 0o700])
        return d
    }

    private static func load() -> Curve25519.KeyAgreement.PrivateKey {
        let url = dir.appendingPathComponent("identity.key")
        if FileManager.default.fileExists(atPath: url.path) {
            if let raw = try? Data(contentsOf: url),
               let key = try? Curve25519.KeyAgreement.PrivateKey(rawRepresentation: raw) {
                return key
            }
            // Unreadable: keep it aside (never silently overwrite an identity), then start fresh.
            let aside = setAside(url)
            Log.info("security: identity.key was unreadable; kept it as \(aside) and created a new identity "
                     + "(paired PCs will need to pair again)")
        }
        let key = Curve25519.KeyAgreement.PrivateKey()
        if !writeAtomically(key.rawRepresentation, to: url) {
            Log.info("security: couldn't save the new identity; it will change on next launch")
        } else {
            Log.info("security: created a new device identity")
        }
        return key
    }

    /// Moves a damaged file out of the way, keeping it for inspection. Returns its new name.
    static func setAside(_ url: URL) -> String {
        let stamp = Int(Date().timeIntervalSince1970)
        let aside = url.deletingLastPathComponent().appendingPathComponent("\(url.lastPathComponent).bad-\(stamp)")
        try? FileManager.default.moveItem(at: url, to: aside)
        return aside.lastPathComponent
    }

    /// Writes via a temp file + rename (never a half-written file), owner-only permissions.
    static func writeAtomically(_ data: Data, to url: URL) -> Bool {
        let tmp = url.appendingPathExtension("tmp")
        guard FileManager.default.createFile(atPath: tmp.path, contents: data, attributes: [.posixPermissions: 0o600])
        else { return false }
        return rename(tmp.path, url.path) == 0
    }
}

/// Short, stable identifier for a public key (16 hex chars), used in UIs and config files.
func deviceID(_ publicKey: Data) -> String {
    SHA512.hash(data: publicKey).prefix(8).map { String(format: "%02x", $0) }.joined()
}

/// PCs the user approved, persisted as JSON next to the identity.
final class TrustStore {
    struct Entry: Codable, Equatable {
        var name: String
        var publicKey: Data
        var added: Date
        var id: String { deviceID(publicKey) }
    }

    static let shared = TrustStore()
    private let lock = NSLock()
    private var entries: [Entry]
    private let url = Identity.dir.appendingPathComponent("trusted-pcs.json")

    private init() {
        entries = []
        if FileManager.default.fileExists(atPath: url.path) {
            if let data = try? Data(contentsOf: url), let list = try? JSONDecoder().decode([Entry].self, from: data) {
                entries = list
            } else {
                // Don't overwrite a list we couldn't read on the next save; keep it for recovery.
                let aside = Identity.setAside(url)
                Log.info("security: trusted-pcs.json was unreadable; kept it as \(aside). PCs will need to pair again.")
            }
        }
    }

    var all: [Entry] { lock.lock(); defer { lock.unlock() }; return entries }

    func isTrusted(_ key: Data) -> Bool { all.contains { $0.publicKey == key } }

    @discardableResult
    func add(name: String, key: Data) -> Bool {
        lock.lock(); defer { lock.unlock() }
        entries.removeAll { $0.publicKey == key }
        entries.append(Entry(name: name, publicKey: key, added: Date()))
        return save()
    }

    /// Returns false if the change couldn't be saved (it would come back after a restart).
    @discardableResult
    func remove(key: Data) -> Bool {
        lock.lock(); defer { lock.unlock() }
        entries.removeAll { $0.publicKey == key }
        return save()
    }

    private func save() -> Bool {
        guard let data = try? JSONEncoder().encode(entries), Identity.writeAtomically(data, to: url) else {
            Log.info("security: couldn't save trusted-pcs.json")
            return false
        }
        return true
    }
}

/// Handshake and record layer. See docs/PROTOCOL.md.
///
/// Handshake (plaintext, each message prefixed with u32 LE length):
///   C→H ClientHello: "SKVM", u16 proto, u16 0, 32 static pub, 32 ephemeral pub, str name, str version, u8 platform
///   H→C HostHello:   "SKVM", u16 proto, u16 status, 32 static pub, 32 ephemeral pub, str name, str version, u8 platform
/// Keys: HKDF-SHA512(ikm = X25519(ce,he) ‖ X25519(cs,he) ‖ X25519(ce,hs), salt = SHA512(ClientHello ‖ HostHello),
///       info = "SoftKVM v1 session") → 32 c→h key, 32 h→c key, 4 bytes for the 6-digit pairing code.
/// Records: u32 LE length (ciphertext + 16) ‖ ChaCha20-Poly1305(ciphertext ‖ tag), nonce = 0u32 ‖ u64 LE counter,
///          associated data = the 4 length bytes.
enum Handshake {
    static let protocolVersion: UInt16 = 2
    static let magic = Data("SKVM".utf8)
    static let maxMessage = 4096

    enum Platform: UInt8 { case windows = 1, mac = 2, linux = 3 }
    enum Status: UInt16 { case ok = 0, versionMismatch = 1 }

    struct Hello {
        var proto: UInt16
        var status: UInt16
        var staticKey: Data
        var ephemeralKey: Data
        var name: String
        var version: String
        var platform: UInt8
    }

    /// Commitment (v2): "SKVM", u16 proto, u16 0, SHA-512 of the ClientHello the client will send.
    struct Commit { var proto: UInt16; var hash: Data }

    static func decodeCommit(_ d: Data) -> Commit? {
        guard d.count == 4 + 4 + 64, d.prefix(4) == magic, let proto = d.readLE(UInt16.self, at: 4) else { return nil }
        return Commit(proto: proto, hash: d.subdata(in: (d.startIndex + 8)..<(d.startIndex + 72)))
    }

    /// A peer-supplied name made safe to show: no control characters, at most 40 characters.
    static func displayName(_ raw: String) -> String {
        let cleaned = String(raw.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) }
                                .map(Character.init))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let short = cleaned.count > 40 ? String(cleaned.prefix(40)) + "…" : cleaned
        return short.isEmpty ? "PC" : short
    }

    static func encode(_ h: Hello) -> Data {
        var d = Data(magic)
        d.appendLE(h.proto)
        d.appendLE(h.status)
        d.append(h.staticKey)
        d.append(h.ephemeralKey)
        appendString(&d, h.name)
        appendString(&d, h.version)
        d.append(h.platform)
        return d
    }

    static func decode(_ d: Data) -> Hello? {
        guard d.count >= 4 + 4 + 64 + 3, d.prefix(4) == magic else { return nil }
        var o = 4
        guard let proto = d.readLE(UInt16.self, at: o), let status = d.readLE(UInt16.self, at: o + 2) else { return nil }
        o += 4
        let s = d.subdata(in: (d.startIndex + o)..<(d.startIndex + o + 32)); o += 32
        let e = d.subdata(in: (d.startIndex + o)..<(d.startIndex + o + 32)); o += 32
        guard let name = readString(d, &o), let version = readString(d, &o), o < d.count else { return nil }
        let platform = d[d.startIndex + o]
        return Hello(proto: proto, status: status, staticKey: s, ephemeralKey: e, name: name, version: version,
                     platform: platform)
    }

    private static func appendString(_ d: inout Data, _ s: String) {
        let b = Array(s.utf8.prefix(255))
        d.append(UInt8(b.count))
        d.append(contentsOf: b)
    }

    private static func readString(_ d: Data, _ o: inout Int) -> String? {
        guard o < d.count else { return nil }
        let n = Int(d[d.startIndex + o]); o += 1
        guard o + n <= d.count else { return nil }
        let s = String(decoding: d[(d.startIndex + o)..<(d.startIndex + o + n)], as: UTF8.self)
        o += n
        return s
    }

    struct Keys {
        var receive: SymmetricKey
        var send: SymmetricKey
        var pairingCode: String
    }

    /// Host side key derivation.
    static func hostKeys(clientHello: Data, hostHello: Data, client: Hello,
                         hostEphemeral: Curve25519.KeyAgreement.PrivateKey) throws -> Keys {
        let ce = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: client.ephemeralKey)
        let cs = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: client.staticKey)
        let ee = try hostEphemeral.sharedSecretFromKeyAgreement(with: ce)
        let se = try hostEphemeral.sharedSecretFromKeyAgreement(with: cs)        // client static × host ephemeral
        let es = try Identity.shared.sharedSecretFromKeyAgreement(with: ce)      // client ephemeral × host static
        var ikm = Data()
        for s in [ee, se, es] { s.withUnsafeBytes { ikm.append(contentsOf: $0) } }
        let salt = Data(SHA512.hash(data: clientHello + hostHello))
        let okm = HKDF<SHA512>.deriveKey(inputKeyMaterial: SymmetricKey(data: ikm), salt: salt,
                                         info: Data("SoftKVM v1 session".utf8), outputByteCount: 68)
        let bytes = okm.withUnsafeBytes { Data($0) }
        let c2h = SymmetricKey(data: bytes[0..<32])
        let h2c = SymmetricKey(data: bytes[32..<64])
        let code = bytes.readLE(UInt32.self, at: 64)! % 1_000_000
        return Keys(receive: c2h, send: h2c, pairingCode: String(format: "%03d %03d", code / 1000, code % 1000))
    }
}

/// Encrypts/decrypts records with per-direction nonce counters. Thread-safe for sending.
final class RecordCipher {
    private let sendKey: SymmetricKey
    private let receiveKey: SymmetricKey
    private var sendCounter: UInt64 = 0
    private var receiveCounter: UInt64 = 0
    private let lock = NSLock()

    init(keys: Handshake.Keys) {
        sendKey = keys.send
        receiveKey = keys.receive
    }

    private static func nonce(_ counter: UInt64) -> ChaChaPoly.Nonce {
        var n = Data(count: 4)
        n.appendLE(counter)
        return try! ChaChaPoly.Nonce(data: n)
    }

    /// Encrypts one plaintext message and passes the framed record to `emit` while holding the
    /// lock, so records hit the socket in nonce order.
    func seal(_ plain: Data, emit: (Data) -> Void) {
        lock.lock()
        defer { lock.unlock() }
        var header = Data()
        header.appendLE(UInt32(plain.count + 16))
        let box = try! ChaChaPoly.seal(plain, using: sendKey, nonce: Self.nonce(sendCounter), authenticating: header)
        sendCounter += 1
        var record = header
        record.reserveCapacity(4 + plain.count + 16)
        record.append(box.ciphertext)
        record.append(box.tag)
        emit(record)
    }

    /// Decrypts one record body (ciphertext ‖ tag); `length` is its 4-byte header. Receive side is single-threaded.
    func open(header: Data, body: Data) -> Data? {
        guard body.count >= 16 else { return nil }
        let ct = body.prefix(body.count - 16), tag = body.suffix(16)
        guard let box = try? ChaChaPoly.SealedBox(nonce: Self.nonce(receiveCounter), ciphertext: ct, tag: tag),
              let plain = try? ChaChaPoly.open(box, using: receiveKey, authenticating: header) else { return nil }
        receiveCounter += 1
        return plain
    }
}
