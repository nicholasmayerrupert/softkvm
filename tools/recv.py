#!/usr/bin/env python3
"""Dev receiver: connects to a SoftKVM host, requests displays, and dumps each stream
to an Annex B file (out/stream<N>.h265) for inspection with ffprobe/ffmpeg.

  python3 tools/recv.py HOST [--seconds 10] [--display 2560x1440@60 ...]
"""
import argparse, os, socket, struct, time

HELLO, KEYREQ = 0x81, 0x82
HELLO_ACK, STREAM_INFO, VIDEO = 0x01, 0x02, 0x03
CODECS = {1: "h265", 2: "h264"}


def msg(t, stream=0, payload=b""):
    return struct.pack("<BBHI", t, stream, 0, len(payload)) + payload


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("host closed connection")
        buf += chunk
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=47900)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--display", action="append", default=[])
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out"))
    a = ap.parse_args()
    displays = a.display or ["2560x1440@60"]
    os.makedirs(a.out, exist_ok=True)

    s = socket.create_connection((a.host, a.port), timeout=15)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    hello = struct.pack("<II", 0, len(displays))
    for d in displays:
        wh, fps = d.split("@")
        w, h = wh.split("x")
        hello += struct.pack("<III", int(w), int(h), int(fps))
    s.sendall(msg(HELLO, payload=hello))

    files, stats = {}, {}
    start = time.time()
    first_video = None
    try:
        while time.time() - start < a.seconds:
            t, stream, _, n = struct.unpack("<BBHI", recv_exact(s, 8))
            p = recv_exact(s, n)
            if t == HELLO_ACK:
                print("hello ack, version", struct.unpack("<I", p[:4])[0])
            elif t == STREAM_INFO:
                w, h, fps, codec = struct.unpack("<IIII", p[:16])
                ext = CODECS.get(codec, "bin")
                print(f"stream {stream}: {w}x{h}@{fps} {ext}")
                files[stream] = open(os.path.join(a.out, f"stream{stream}.{ext}"), "wb")
                stats[stream] = [0, 0, 0]  # frames, bytes, keyframes
            elif t == VIDEO:
                if first_video is None:
                    first_video = time.time()
                    print(f"first frame after {first_video - start:.2f}s")
                pts, flags = struct.unpack("<QB", p[:9])
                files[stream].write(p[9:])
                st = stats[stream]
                st[0] += 1; st[1] += len(p) - 9; st[2] += flags & 1
    except socket.timeout:
        print("timed out waiting for data")
    finally:
        s.close()
        for f in files.values():
            f.close()
    dur = time.time() - (first_video or start)
    for k, (fr, by, kf) in stats.items():
        print(f"stream {k}: {fr} frames ({fr / max(dur, 1e-6):.1f} fps), {kf} keyframes, "
              f"{by * 8 / max(dur, 1e-6) / 1e6:.1f} Mbps over {dur:.1f}s")


if __name__ == "__main__":
    main()
