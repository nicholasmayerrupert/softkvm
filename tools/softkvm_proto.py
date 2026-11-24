"""SoftKVM v1 client protocol in Python (reference implementation + test tooling).

See docs/PROTOCOL.md. Requires the `cryptography` package.
"""
import hashlib
import os
import socket
import struct

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

PROTO = 1
INFO = b"SoftKVM v1 session"
PLATFORM_WINDOWS, PLATFORM_MAC, PLATFORM_LINUX = 1, 2, 3

# message types (inside the encrypted channel)
HELLO_ACK, STREAM_INFO, VIDEO, AUDIO, PING, LAYOUT, CURSOR, CURSOR_POS = 1, 2, 3, 4, 5, 6, 7, 8
SET_MONITORS, BYE, AUTH_STATUS = 9, 0x0A, 0x0B
HELLO, KEYFRAME_REQUEST, MONITORS, TAKEOVER, GOODBYE, INPUT = 0x81, 0x82, 0x83, 0x84, 0x85, 0x90
AUTH_NAMES = {0: "ok", 1: "pairing pending", 2: "pairing denied", 3: "busy", 4: "paired"}


def raw(priv):
    return priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                              serialization.NoEncryption())


def pub_raw(priv):
    return priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def device_id(pub):
    return hashlib.sha512(pub).digest()[:8].hex()


def encode_hello(proto, status, static_pub, eph_pub, name, version, platform):
    n, v = name.encode()[:255], version.encode()[:255]
    return (b"SKVM" + struct.pack("<HH", proto, status) + static_pub + eph_pub
            + bytes([len(n)]) + n + bytes([len(v)]) + v + bytes([platform]))


def decode_hello(b):
    if len(b) < 75 or b[:4] != b"SKVM":
        raise ValueError("not a SoftKVM hello")
    proto, status = struct.unpack_from("<HH", b, 4)
    o = 8
    s, e = b[o:o + 32], b[o + 32:o + 64]
    o += 64
    n = b[o]; name = b[o + 1:o + 1 + n].decode(errors="replace"); o += 1 + n
    n = b[o]; version = b[o + 1:o + 1 + n].decode(errors="replace"); o += 1 + n
    return dict(proto=proto, status=status, static=s, eph=e, name=name, version=version, platform=b[o])


def derive_client(client_hello, host_hello, c_static, c_eph, host):
    hs, he = X25519PublicKey.from_public_bytes(host["static"]), X25519PublicKey.from_public_bytes(host["eph"])
    ikm = c_eph.exchange(he) + c_static.exchange(he) + c_eph.exchange(hs)
    salt = hashlib.sha512(client_hello + host_hello).digest()
    okm = HKDF(algorithm=hashes.SHA512(), length=68, salt=salt, info=INFO).derive(ikm)
    code = struct.unpack_from("<I", okm, 64)[0] % 1_000_000
    return okm[:32], okm[32:64], f"{code // 1000:03d} {code % 1000:03d}"


def nonce(ctr):
    return b"\0\0\0\0" + struct.pack("<Q", ctr)


class Channel:
    """Encrypted client connection to a SoftKVM host."""

    def __init__(self, host, port=47900, identity=None, name="python-client", version="dev", timeout=15):
        self.static = identity or X25519PrivateKey.generate()
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        eph = X25519PrivateKey.generate()
        ch = encode_hello(PROTO, 0, pub_raw(self.static), pub_raw(eph), name, version, PLATFORM_LINUX)
        self.sock.sendall(struct.pack("<I", len(ch)) + ch)
        (n,) = struct.unpack("<I", self._recv(4))
        hh = self._recv(n)
        self.host = decode_hello(hh)
        if self.host["status"] != 0 or self.host["proto"] != PROTO:
            raise RuntimeError(f"version mismatch: host speaks v{self.host['proto']} (app {self.host['version']})")
        k_send, k_recv, self.code = derive_client(ch, hh, self.static, eph, self.host)
        self.send_aead, self.recv_aead = ChaCha20Poly1305(k_send), ChaCha20Poly1305(k_recv)
        self.send_ctr = self.recv_ctr = 0

    def _recv(self, n):
        buf = bytearray()
        while len(buf) < n:
            c = self.sock.recv(n - len(buf))
            if not c:
                raise ConnectionError("host closed the connection")
            buf += c
        return bytes(buf)

    def send(self, mtype, payload=b"", stream=0):
        plain = struct.pack("<BBHI", mtype, stream, 0, len(payload)) + payload
        header = struct.pack("<I", len(plain) + 16)
        ct = self.send_aead.encrypt(nonce(self.send_ctr), plain, header)
        self.send_ctr += 1
        self.sock.sendall(header + ct)

    def recv(self):
        header = self._recv(4)
        (n,) = struct.unpack("<I", header)
        plain = self.recv_aead.decrypt(nonce(self.recv_ctr), self._recv(n), header)
        self.recv_ctr += 1
        mtype, stream, _, ln = struct.unpack_from("<BBHI", plain)
        return mtype, stream, plain[8:8 + ln]

    def close(self):
        self.sock.close()


def save_identity(path, priv):
    with open(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as f:
        f.write(raw(priv))


def load_identity(path):
    if os.path.exists(path):
        with open(path, "rb") as f:
            return X25519PrivateKey.from_private_bytes(f.read())
    priv = X25519PrivateKey.generate()
    save_identity(path, priv)
    return priv
