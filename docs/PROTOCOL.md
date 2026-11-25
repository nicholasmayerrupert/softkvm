# SoftKVM protocol (v2)

All integers are little-endian. "str" means `u8 length` followed by that many UTF-8 bytes.
The **host** is the controlled machine (a Mac today); the **client** is the PC.

## Discovery (UDP 47901)

The client broadcasts `SOFTKVM_DISCOVER` (to 255.255.255.255 and each adapter's directed broadcast).
Hosts reply to the sender:

```
"SOFTKVM_HOST" | u16 tcp_port | u16 protocol | u8 platform | 32-byte public key | str name
```

Platforms: 1 Windows, 2 Mac, 3 Linux. Discovery is unauthenticated: it's only used to list devices
and refresh addresses. The key it announces must still be proven in the handshake.

## Handshake (TCP 47900)

Each side has a long-term X25519 identity. Every connection starts with two plaintext messages,
each prefixed with a `u32` length (at most 4096):

```
Hello = "SKVM" | u16 protocol | u16 status | 32 static public key | 32 ephemeral public key
        | str name | str app_version | u8 platform
```

1. Client → host: **commitment** = `"SKVM" | u16 protocol | u16 0 | SHA-512(ClientHello)` (72 bytes).
2. Host → client: `HostHello`. Status 1 = version mismatch (the host closes; the client shows "update
   needed" with both versions). A first message that isn't a v2 commitment (e.g. a v1 ClientHello)
   gets this answer.
3. Client → host: `ClientHello` (status 0). The host closes unless its SHA-512 equals the commitment.

Because the client commits to its hello before seeing the host's, and the host sends its hello before
seeing the client's, neither side can try many hellos to make the 6-digit pairing code match another
connection's (a man in the middle would otherwise grind ~10⁶ candidates).

Both sides then derive keys:

```
ikm  = X25519(client_eph, host_eph) ‖ X25519(client_static, host_eph) ‖ X25519(client_eph, host_static)
salt = SHA-512(ClientHello ‖ HostHello)            (message bodies, without length prefixes)
okm  = HKDF-SHA512(ikm, salt, info = "SoftKVM v1 session", 68 bytes)
client→host key = okm[0:32]   host→client key = okm[32:64]
pairing code    = u32(okm[64:68]) mod 1 000 000, shown as "123 456"
```

**Key confirmation.** The client's first record must be `CONFIRM`, and the host's first is
`AUTH_STATUS`. Only a party holding the static private key it advertised can derive the keys, so these
two records prove both identities. The host decides nothing about a client until its `CONFIRM`
decrypts: knowing a paired PC's *public* key gets an impostor nowhere. Clients pin a device's key once paired. A device presenting a
different key is refused ("its identity changed").

## Records

After the handshake, every message travels in a record:

```
u32 length (= plaintext length + 16) | ChaCha20-Poly1305 ciphertext | 16-byte tag
nonce = 4 zero bytes ‖ u64 counter (per direction, starting at 0)
associated data = the 4 length bytes
```

A record that fails authentication ends the connection.

## Messages (inside records)

```
u8 type | u8 stream | u16 reserved | u32 payload_length | payload
```

### Key confirmation (first client message)

`CONFIRM (0x86)`: `u8 flags`. Bit 0 = *pair with me*: the client hasn't confirmed this device with its
user yet, so the host shows its pairing prompt even if it already trusts the client. The client then
also asks its own user to confirm the code, and refuses a host that answers `OK` without `PAIRED`.

### Authorization (first host message)

`AUTH_STATUS (0x0B)`: `u8 code | str text`

| code | meaning |
|---|---|
| 0 OK | Trusted and in control: send `HELLO` + `MONITORS`. |
| 1 PAIRING_PENDING | Unknown client: the host is asking its user to approve (compare the pairing code). |
| 2 PAIRING_DENIED | Refused. The host closes. |
| 3 BUSY | Another client is in control (`text` = its name). Send `TAKEOVER` to take over, or wait: the host sends `OK` when it's free. |
| 4 PAIRED | Approved just now (informational). `OK` or `BUSY` follows. |

Before `OK`, the host only accepts `TAKEOVER` and `GOODBYE`.

### Host → client

| type | name | payload |
|---|---|---|
| `0x01` | HELLO_ACK | `u32 protocol, str host name` |
| `0x02` | STREAM_INFO | `u32 width, height, fps, codec` (1 HEVC, 2 H.264) |
| `0x03` | VIDEO | `u64 pts_us, u8 flags` (bit 0 = keyframe), Annex B access unit |
| `0x04` | AUDIO | `u64 pts_us`, s16le stereo PCM @ 48 kHz |
| `0x05` | PING | every second, optionally `u64 host_us` (host clock, starts time sync). Clients reconnect after ~4 s of silence. |
| `0x06` | LAYOUT | `u32 n`, n × `u8 stream (0xFF = not streamed), u8[3], i32 x, y, w, h`: every host display, global points |
| `0x07` | CURSOR | `i32 hot_x, hot_y, u32 w, h`, w×h RGBA (straight alpha): on shape change |
| `0x08` | CURSOR_POS | `f32 x, y` global points: when something else moved the cursor |
| `0x09` | SET_MONITORS | `u32 mask`: the host's user picked these client monitors (bit i = monitor i). The client reconnects with them. |
| `0x0A` | BYE | `u8 reason, str text`: 0 disconnected by the host's user, 1 taken over (text = by whom), 2 unpaired, 3 host quitting. Except for 3, the client doesn't reconnect until its user asks. |
| `0x0B` | AUTH_STATUS | see above |
| `0x20` | FRAME_TIMING | latency instrumentation, see docs/DEVELOPMENT.md |
| `0x21` | TIME_RESPONSE | `u64 client_us` (echo), `u64 host_us` |

### Client → host

| type | name | payload |
|---|---|---|
| `0x81` | HELLO | `u32 protocol, u32 n`, n × `u32 width, height, fps`, then optionally n × `i32 x, y` (each monitor's desktop origin, so the host can arrange its displays alike) |
| `0x82` | KEYFRAME_REQUEST | (empty; `stream` selects the display) |
| `0x83` | MONITORS | `str client name, u8 n`, n × `u8 index, u8 selected, u32 w, h, hz, str name`: every client monitor |
| `0x84` | TAKEOVER | (empty) after `BUSY`: take control from the client in control |
| `0x86` | CONFIRM | `u8 flags`, first record (see above) |
| `0x87` | VIEWING | `u8 viewing`: 1 showing the streamed desktop, 0 in PC mode. Optional, ignored by earlier hosts. |
| `0xA0` | TIME_REQUEST | `u64 client_us`. Clients send it every second once authorized; the host drops an authorized client silent for 5 s. |
| `0x85` | GOODBYE | `u8 reason`: 0 quitting (host removes its displays), 1 switching to another device (host keeps them for a while) |
| `0x90` | INPUT | `u8 kind` + fields, see below |

INPUT kinds: 1 MOUSE_MOVE `u8 stream, i32 x, y` (legacy) · 2 MOUSE_BUTTON `u8 button (0 L, 1 R, 2 M,
3 back, 4 forward), u8 down` · 3 SCROLL `f32 dx, dy` (notches; dy > 0 = up) · 4 KEY `u16 mac keycode,
u8 down, u8 repeat` · 5 RELEASE_ALL · 6 MOUSE_MOVE_GLOBAL `f32 x, y` (global host points).

## Reference implementations

- Host: `mac-host/Sources/SoftKVMHost/Security.swift` (CryptoKit) and `Server.swift`.
- Client: `pc-client/src/secure.c` (Monocypher) and `main.c`.
- Python: `tools/softkvm_proto.py` (`cryptography`). The C and Python key derivation and records have
  been checked byte for byte against each other, and Python against the Swift host.
