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
