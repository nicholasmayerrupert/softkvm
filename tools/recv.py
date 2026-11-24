#!/usr/bin/env python3
"""Dev/agent test client for SoftKVM hosts (protocol v1, encrypted).

Connects as a "PC", pairs if needed (approve it on the host), optionally takes over from the
PC in control, asks for virtual displays and dumps each stream to out/stream<N>.h265.

  python3 tools/recv.py HOST [--port 47900] [--identity tools/out/test-identity.key]
                             [--seconds 10] [--display 2560x1440@60 ...] [--takeover] [--no-stream]

The identity file makes the host recognize this client next time (it's created on first use).
Remove it from the host's menu (Paired PCs → Forget) when you're done testing.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from softkvm_proto import (AUDIO, AUTH_NAMES, AUTH_STATUS, BYE, CURSOR, GOODBYE, HELLO, HELLO_ACK, LAYOUT,
                           PING, STREAM_INFO, TAKEOVER, VIDEO, Channel, device_id, load_identity)

CODECS = {1: "h265", 2: "h264"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=47900)
    ap.add_argument("--identity", default=os.path.join(os.path.dirname(__file__), "out", "test-identity.key"))
    ap.add_argument("--name", default="SoftKVM test client")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--display", action="append", default=[])
    ap.add_argument("--takeover", action="store_true", help="take control if another PC has it")
    ap.add_argument("--no-stream", action="store_true", help="authenticate only; don't request displays")
    ap.add_argument("--approval-timeout", type=float, default=120)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    displays = a.display or ["2560x1440@60"]

    ch = Channel(a.host, a.port, identity=load_identity(a.identity), name=a.name, version="test",
                 timeout=a.approval_timeout)
    print(f"connected to {ch.host['name']} (SoftKVM {ch.host['version']}, device {device_id(ch.host['static'])}); "
          f"pairing code {ch.code}", flush=True)

    files, stats, start, first_video = {}, {}, None, None
    try:
        while True:
            if start and time.time() - start >= a.seconds:
                break
            t, stream, p = ch.recv()
            if t == PING:
                continue
            if t == AUTH_STATUS:
                text = p[2:2 + p[1]].decode(errors="replace") if len(p) > 1 else ""
                status = AUTH_NAMES.get(p[0], p[0])
                print(f"auth: {status} {text}".rstrip(), flush=True)
                if status == "busy" and a.takeover:
                    print("taking over", flush=True)
                    ch.send(TAKEOVER)
                elif status == "pairing denied":
                    return 1
                elif status == "ok":
                    if a.no_stream:
                        start = time.time()
                        continue
                    hello = struct.pack("<II", 1, len(displays))
                    for d in displays:
                        wh, fps = d.split("@")
                        w, h = wh.split("x")
                        hello += struct.pack("<III", int(w), int(h), int(fps))
                    ch.send(HELLO, hello)
                    start = time.time()
            elif t == BYE:
                print(f"bye: reason {p[0]} {p[2:2 + p[1]].decode(errors='replace')}", flush=True)
                return 0
            elif t == HELLO_ACK:
                print("streaming", flush=True)
            elif t == STREAM_INFO:
                w, h, fps, codec = struct.unpack_from("<IIII", p)
                print(f"stream {stream}: {w}x{h}@{fps} {CODECS.get(codec)}", flush=True)
                files[stream] = open(os.path.join(a.out, f"stream{stream}.{CODECS.get(codec, 'bin')}"), "wb")
                stats[stream] = [0, 0, 0]
            elif t == VIDEO and stream in files:
                first_video = first_video or time.time()
                files[stream].write(p[9:])
                st = stats[stream]
                st[0] += 1; st[1] += len(p) - 9; st[2] += p[8] & 1
    finally:
        try:
            ch.send(GOODBYE, b"\0")  # leaving on purpose: the host removes our displays
        except Exception:
            pass
        ch.close()
        for f in files.values():
            f.close()
    dur = time.time() - (first_video or start or time.time())
    for k, (fr, by, kf) in stats.items():
        print(f"stream {k}: {fr} frames ({fr / max(dur, 1e-6):.1f} fps), {kf} keyframes, "
              f"{by * 8 / max(dur, 1e-6) / 1e6:.1f} Mbps over {dur:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
