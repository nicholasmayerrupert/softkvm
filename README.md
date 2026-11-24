# SoftKVM

A software KVM: use your PC's monitors, keyboard and mouse to control your other computers (Macs today;
Linux is planned).

Each Mac creates virtual displays matching the PC's monitors and streams them, hardware-encoded, to
fullscreen windows on those monitors. The PC draws the cursor locally and sends keyboard and mouse
back. One hotkey flips between the PC and the device you're controlling; another picks the device.

```
 ┌──────────── Mac ────────────┐            ┌──────────── PC ──────────────┐
 │ virtual display 1 ─┐        │ encrypted  │        ┌─ fullscreen window 1 │
 │ virtual display 2 ─┤ HEVC ──┼───────────►│ decode ┤                      │
 │ system audio       │encoder │    TCP     │ (GPU)  └─ fullscreen window 2 │
 │ CGEvent injection ◄┼────────┼────────────┤ keyboard/mouse, local cursor │
 └─────────────────────────────┘            └──────────────────────────────┘
```

## Install

| Machine | Get | Then |
|---|---|---|
| **Mac** (macOS 14+, Apple Silicon) | `SoftKVM-Host-<version>.dmg` (`mac-host/package.sh`) | Drag **SoftKVM Host** to Applications, open it, grant **Screen Recording** and **Accessibility**. Turn on **Launch at Login** from its menu. |
| **PC** (Windows 10/11) | `SoftKVM-Setup-<version>.exe` (`pc-client/package.sh`) | Run it (no admin needed). It installs to `%LOCALAPPDATA%\Programs\SoftKVM`, adds a Start menu entry and (optionally) starts with Windows. |

The builds aren't signed with a distribution certificate yet, so on a new machine Windows SmartScreen
("More info → Run anyway") and macOS Gatekeeper (right-click → Open) will ask once.

## Use

**Pairing.** The first time a PC connects to a Mac, the Mac asks *"Allow ‹PC› to control this Mac?"*
and both show the same 6-digit code. Approve only if the codes match. After that the PC reconnects by
itself. Everything between paired devices is encrypted.

**PC (tray icon near the clock, blue when connected):**
- **Devices:** each paired device with its state (online, connected, in use by another PC, …).
  Click one, or press **Ctrl+Alt+1…9**, to control it.
- **Ctrl+Alt+Space:** switch between this PC and the active device.
- **Monitors for ‹device›:** which PC monitors show that device (one Mac display per checked monitor).
  Remembered per device.
- **Add a device:** devices found on your network. **Forget a device** removes a pairing.
- **Keyboard:** Ctrl acts as Cmd (PC style, default) or the Windows key does (Mac style).
- **Start with Windows**, **Open log**, **Quit**. **Ctrl+Alt+Q** quits while controlling a device.

**Mac (menu-bar icon):** connection status, the PC's monitors (same checkboxes), **Paired PCs**
(Forget), **Video quality**, **Disconnect PC**, **Permissions**, **Launch at Login**, **Open Log**.

**One PC at a time.** If another PC is controlling a Mac, the device shows "in use by ‹PC›"; clicking
it offers to take over. A Mac you disconnect from its menu stays disconnected until you reconnect from
the PC.

**Good to know**
- The Mac keeps its virtual displays for 15 minutes after an unexpected disconnect (PC asleep, network
  blip), so windows stay where they were when the PC comes back.
- The Mac won't idle-sleep while a PC is connected.
- If a device's address changes (e.g. Wi-Fi → Ethernet), the PC finds it again by its identity.
- Wired Ethernet on the Mac makes the biggest difference to latency and smoothness.

## How it works

| Path | What |
|---|---|
| `mac-host/` | Swift menu-bar app. Virtual displays (private `CGVirtualDisplay` API), ScreenCaptureKit capture, VideoToolbox HEVC, system audio, CGEvent input, pairing UI. |
| `pc-client/` | Windows client in C. SDL2 windows per monitor, FFmpeg D3D11VA decode, raw mouse + local cursor, tray UI, NSIS installer. |
| `tools/` | Python implementation of the protocol (`softkvm_proto.py`), a test client (`recv.py`), deploy scripts. |
| `docs/PROTOCOL.md` | Wire protocol: discovery, handshake, encryption, messages. |
| `docs/DEVELOPMENT.md` | Building, deploying and testing (for people and agents). |

Settings: PC `%APPDATA%\SoftKVM\config.ini` and `identity.dat` (DPAPI); Mac
`~/Library/Application Support/SoftKVM/` (`identity.key`, `trusted-pcs.json`). Logs: next to
`softkvm.exe` on the PC, `~/Library/Logs/SoftKVMHost.log` on the Mac.

## Caveats

- Virtual displays rely on Apple's private `CGVirtualDisplay` API (as DeskPad and BetterDisplay do); a
  macOS update could break it.
- After a reboot with FileVault, the Mac must be unlocked at its own keyboard once.
- Closed-lid use hasn't been tested yet; a MacBook normally sleeps when its lid closes.
