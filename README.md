# SoftKVM

A software KVM: use your PC's monitors, keyboard and mouse to drive a Mac.

The Mac creates virtual displays matching the PC's monitors and streams each one, hardware-encoded,
to a fullscreen window on the matching PC monitor. Keyboard and mouse input flows back to the Mac.
One hotkey on the PC flips between "Mac mode" and "PC mode".

```
 ┌──────────── Mac ────────────┐           ┌──────────── PC ─────────────┐
 │ virtual display 1 ─┐        │  LAN/TCP  │        ┌─ fullscreen window 1 │
 │ virtual display 2 ─┤ HEVC ──┼──────────►│ decode ┤                      │
 │                    │encoder │           │ (GPU)  └─ fullscreen window 2 │
 │ CGEvent injection ◄┼────────┼───────────┤ keyboard/mouse capture       │
 └─────────────────────────────┘           └──────────────────────────────┘
```

## Layout

| Path | What |
|---|---|
| `mac-host/` | Swift app (macOS 14+, Apple Silicon). Virtual displays, ScreenCaptureKit capture, VideoToolbox encode, input injection. |
| `pc-client/` | Windows client in C. FFmpeg (D3D11VA) decode, SDL2 windows per monitor, input capture, hotkey. |
| `tools/` | Dev helpers, e.g. a Python receiver that dumps the stream to a file for testing. |

## Status

- [x] Milestone 1: Mac virtual displays → PC windows (tested with two displays)
- [ ] Milestone 2: keyboard/mouse forwarding + Mac/PC hotkey
- [x] Milestone 3: two displays
- [x] Audio (system audio, uncompressed PCM)
- [ ] Milestone 4: clipboard, auto-reconnect, start on login

## Protocol (v0)

One TCP connection, Mac listens on port **47900**. All integers little-endian.

Every message: `u8 type | u8 stream | u16 reserved | u32 payload_len | payload`

Host → client

| type | name | payload |
|---|---|---|
| `0x01` | HELLO_ACK | `u32 version, u8 len, Mac name` |
| `0x02` | STREAM_INFO | `u32 width, u32 height, u32 fps, u32 codec` (1 = HEVC, 2 = H.264) |
| `0x03` | VIDEO | `u64 pts_us, u8 flags` (bit 0 = keyframe), then Annex B bitstream |
| `0x05` | PING | (empty) sent every second; clients reconnect after ~4 s of silence |
| `0x06` | LAYOUT | `u32 n`, then n × `u8 stream (0xFF = not streamed), u8[3], i32 x, y, w, h` — every Mac display in global points |
| `0x07` | CURSOR | `i32 hot_x, hot_y, u32 w, h`, then w×h RGBA (straight alpha) — sent when the cursor shape changes |
| `0x09` | SET_MONITORS | `u32 mask` — the Mac's menu asks the PC to use these monitors (bit i = monitor i); the PC reconnects |
| `0x0A` | BYE | (empty) deliberate disconnect from the Mac's menu; the client stays disconnected until its user reconnects |
| `0x08` | CURSOR_POS | `f32 x, y` global points — when something other than the client moved the cursor |
| `0x04` | AUDIO | `u64 pts_us`, then s16le stereo PCM @ 48 kHz (system audio) |

Client → host

| type | name | payload |
|---|---|---|
| `0x81` | HELLO | `u32 version, u32 n`, then n × `u32 width, u32 height, u32 fps`, then optionally n × `i32 x, y` (each PC monitor's desktop origin, used to arrange the Mac's displays alike) |
| `0x83` | MONITORS | `u8 len, PC name, u8 n`, then n × `u8 index, u8 selected, u32 w, h, hz, u8 len, name` — every PC monitor, sent after HELLO |
| `0x82` | KEYFRAME_REQUEST | (empty; `stream` field selects the display) |
| `0x90` | INPUT | first byte = kind; see `mac-host/Sources/SoftKVMHost/InputInjector.swift` |

**Discovery:** clients broadcast `SOFTKVM_DISCOVER` to UDP **47901**; hosts reply `SOFTKVM_HOST`, `u16` TCP port, then the Mac's name.

## Using it

**Mac:** run **SoftKVM Host** from Applications. Its menu-bar icon shows whether a PC is connected and has:
- **Use these PC monitors for the Mac:** one checkbox per PC monitor. Each checked monitor gets one Mac virtual display.
- **Video quality**, **Disconnect PC**, **Permissions**, **Launch at Login**, **Open Log**, **Quit**.

**PC:** run `softkvm.exe` (no arguments needed). It finds the Mac on the LAN by itself. The tray icon (blue when connected) has:
- **Switch to Mac / PC** (also **Ctrl+Alt+Space**).
- **Mac displays:** the same per-monitor checkboxes.
- **Mac:** choose which Mac to connect to, or search again.
- **Keyboard:** Ctrl as Cmd (PC style) or the Windows key as Cmd (Mac style).
- **Start with Windows**, **Open log**, **Quit** (also **Ctrl+Alt+Q** in Mac mode).

Settings are saved in `%APPDATA%\SoftKVM\config.ini`; the log is next to `softkvm.exe`.

## Caveats

- Virtual displays use Apple's private `CGVirtualDisplay` API (same as DeskPad / BetterDisplay); a macOS update can break it.
- The Mac app needs Screen Recording and (for input) Accessibility permission, granted once.
- Wired Ethernet on both ends is strongly recommended.

## Building

**Mac host** (on the Mac): `mac-host/setup-signing.sh` once, then `mac-host/install.sh`, which builds,
installs `/Applications/SoftKVM Host.app` and launches it. Grant Screen Recording and Accessibility on first launch.

**PC client** (cross-compiled from Linux with mingw-w64): `pc-client/build.sh` → `pc-client/dist/`
(`softkvm.exe` + DLLs). Run `softkvm.exe` (optionally `softkvm.exe <mac-ip>` to skip discovery).

**Test without a PC**: `python3 tools/recv.py <mac-ip> --seconds 10` dumps the stream to
`tools/out/stream0.h265`.

## Latency instrumentation and benchmarks

The host timestamps every frame on its own clock and the client estimates the clock offset,
so each stage can be measured end to end. All of this is ignored by older peers.

| type | dir | name | payload |
|---|---|---|---|
| `0x05` | H→C | PING | now optionally `u64 host_us` (host clock); clients that see it start time sync |
| `0x20` | H→C | FRAME_TIMING | sent before each VIDEO: `u64 pts_us` (matches VIDEO), `u64 capture_us` (ScreenCaptureKit display time), `u64 callback_us`, `u64 encoded_us`, `u64 queued_us`, `u32 unsent_bytes`, `u32 flags` (bit 0 = idle refresh), `u64 submit_us` (handed to the encoder) |
| `0xA0` | C→H | TIME_REQUEST | `u64 client_us` |
| `0x21` | H→C | TIME_RESPONSE | `u64 client_us` (echo), `u64 host_us` |

The client logs a latency line per stream every 5 s (capture→present p50/p95/p99, plus the
host, network and client shares).

**Benchmarking next to a live setup** (nothing visible on either machine):

```
# Mac: a second, invisible host on another port with an animated test pattern
open -n --env SOFTKVM_PORT=47910 --env SOFTKVM_PERF=1 --env SOFTKVM_ANIMATE=1 mac-host/SoftKVMHost.app
# PC: headless client (hidden windows, no audio, no input), 20 s
softkvm.exe MAC --port 47910 --monitor 0:2560x1440@60 --monitor 0:2560x1600@60 --bench 20 --csv run1
python3 tools/perf_analyze.py run1          # p50/p95/p99 per stage, E2E, frame pacing
```

`SOFTKVM_PERF=1` names the test displays "SoftKVM perf N" (own serials), tucks them into a
corner of the arrangement for the app's lifetime only, skips the menu-bar item, input
injection and arrangement mirroring, and logs to `~/Library/Logs/SoftKVMHost-perf.log`.

Host knobs (environment): `SOFTKVM_CODEC=h264|hevc`, `SOFTKVM_MAX_INFLIGHT` (frames inside
the encoder per display, default 1, 0 = unlimited), `SOFTKVM_SCK_FPS` (ScreenCaptureKit
minimum frame interval as fps, default 0 = every frame), `SOFTKVM_SCK_QUEUE_DEPTH`,
`SOFTKVM_SCK_PIXFMT=bgra|420f`, `SOFTKVM_ENC_LOWLAT=0`, `SOFTKVM_NO_AUDIO=1`,
`SOFTKVM_ANIMATE_SPEED` (points/s).

Client flags: `--present flip|tearing|vsync` (default flip), `--sdl-render` (old copy path),
and for benchmarks `--bench N`, `--csv PREFIX`, `--snapshot FILE.bmp`, `--gpu-wait`,
`--decode-sync`.
