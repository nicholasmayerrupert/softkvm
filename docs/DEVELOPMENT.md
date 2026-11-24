# Developing SoftKVM

The PC client is cross-compiled on Linux. The Mac host is built on the Mac. Both are deployed over SSH,
so a person or an agent can work from one Linux machine.

## One-time setup

**Linux build machine**
```sh
sudo apt-get install mingw-w64 nsis ffmpeg python3-cryptography python3-pil
pc-client/fetch-deps.sh          # SDL2 + FFmpeg (LGPL, shared) for Windows → pc-client/deps/
cp tools/deploy.env.example tools/deploy.env   # then fill in the SSH targets (git-ignored)
```

**Mac:** Xcode command line tools, and SSH access for your build machine. On the Mac, run
`mac-host/setup-signing.sh` once. It creates a dedicated keychain with a self-signed code-signing
identity, so builds over SSH are signed consistently and keep their Screen Recording / Accessibility
grants.

**PC:** Windows OpenSSH server enabled, with key-based login for your build machine.

## Everyday loop

| Do | Command |
|---|---|
| Build + install + relaunch the Mac host | `tools/deploy-mac.sh` |
| Build the client, copy it over the installed app, restart it | `tools/deploy-pc.sh` (`HIDDEN=1` to start in PC mode) |
| Same, through the real installer | `tools/deploy-pc.sh --installer` |
| Windows installer | `pc-client/package.sh` → `pc-client/dist-installer/SoftKVM-Setup-<version>.exe` |
| Mac disk image (on the Mac) | `mac-host/package.sh` → `mac-host/dist/SoftKVM-Host-<version>.dmg` |
| Regenerate the app icon | `python3 assets/make_icon.py` |

The version for both apps comes from `VERSION`. Bump `Handshake.protocolVersion` (Swift) and
`SK_PROTO_VERSION` (C) together when the wire protocol changes incompatibly. Mismatched peers then
show a clear "update needed".

The client log is `%LOCALAPPDATA%\Programs\SoftKVM\softkvm.log`; the host log is
`~/Library/Logs/SoftKVMHost.log`.

## Testing without touching the live setup

- **A second host on the Mac:** `open -n --env SOFTKVM_PORT=47920 --env SOFTKVM_HEADLESS=1 <app>`.
  Headless means no menu-bar item and no discovery. It shares the Mac's identity and paired PCs.
- **A test client from Linux:**
  `python3 tools/recv.py <mac> [--port 47920] [--takeover] [--seconds 10] [--display 2560x1440@60]`.
  It pairs on first use (approve on the Mac), then dumps streams to `tools/out/`. Forget it in the Mac
  menu afterwards.
- **Starting GUI apps on Windows over SSH:** SSH sessions have no desktop. `deploy-pc.sh` launches the
  client through a scheduled task with an Interactive principal, which runs it in the signed-in
  user's session.
- Don't kill every `softkvm.exe` on the PC: other builds (e.g. benchmarks) may be running. Stop only
  the copy you deployed.

## Latency instrumentation and benchmarks

The host timestamps every frame on its own clock and the client estimates the clock offset, so every
stage can be measured. The client logs a latency line per stream every 5 s (capture→present
p50/p95/p99, plus the host, network and client shares). Messages (inside the encrypted channel):

| type | dir | name | payload |
|---|---|---|---|
| `0x05` | H→C | PING | optionally `u64 host_us` (host clock); clients that see it start time sync |
| `0x20` | H→C | FRAME_TIMING | sent before each VIDEO: `u64 pts_us`, `u64 capture_us` (ScreenCaptureKit display time), `u64 callback_us`, `u64 encoded_us`, `u64 queued_us`, `u32 unsent_bytes`, `u32 flags` (bit 0 = idle refresh), `u64 submit_us` |
| `0xA0` | C→H | TIME_REQUEST | `u64 client_us` |
| `0x21` | H→C | TIME_RESPONSE | `u64 client_us` (echo), `u64 host_us` |

**Benchmarking next to a live setup** (nothing visible on either machine):

```
# Mac: a second, invisible host on another port with an animated test pattern
open -n --env SOFTKVM_PORT=47910 --env SOFTKVM_PERF=1 --env SOFTKVM_ANIMATE=1 mac-host/SoftKVMHost.app
# PC: headless client (hidden windows, no audio, no input, settings untouched), 20 s
softkvm.exe MAC --port 47910 --monitor 0:2560x1440@60 --monitor 0:2560x1600@60 --bench 20 --csv run1
python3 tools/perf_analyze.py run1          # p50/p95/p99 per stage, E2E, frame pacing
```

The benchmark client uses the PC's own identity, so a PC that is already paired with the Mac needs no
new approval. `SOFTKVM_PERF=1` names the test displays "SoftKVM perf N" (own serials), tucks them into
a corner of the arrangement while the app runs, skips the menu-bar item, LAN discovery, input
injection and arrangement mirroring, and logs to `~/Library/Logs/SoftKVMHost-perf.log`.

Host knobs (environment): `SOFTKVM_CODEC=h264|hevc`, `SOFTKVM_MAX_INFLIGHT` (frames inside the
encoder per display, default 1, 0 = unlimited), `SOFTKVM_SCK_FPS` (ScreenCaptureKit minimum frame
interval as fps, default 0 = every frame), `SOFTKVM_SCK_QUEUE_DEPTH`, `SOFTKVM_SCK_PIXFMT=bgra|420f`,
`SOFTKVM_ENC_LOWLAT=0`, `SOFTKVM_NO_AUDIO=1`, `SOFTKVM_ANIMATE_SPEED` (points/s).

Client flags: `--present flip|tearing|vsync` (default flip), `--sdl-render` (old copy-based renderer),
and for benchmarks `--bench N`, `--csv PREFIX`, `--snapshot FILE.bmp`, `--gpu-wait`, `--decode-sync`.
