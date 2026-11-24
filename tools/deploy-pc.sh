#!/bin/bash
# Dev/agent deploy: builds the Windows client here (mingw-w64), copies it into the installed location
# (%LOCALAPPDATA%\Programs\SoftKVM) on the PC and restarts it in the user's desktop session.
#   tools/deploy-pc.sh              copy dist/ over the installed app and restart it
#   tools/deploy-pc.sh --installer  run the real installer silently instead (tests the installer)
#   HIDDEN=1 tools/deploy-pc.sh     restart it in the background (PC mode) instead of showing the Mac
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/deploy.env"
PC() { ssh $PC_SSH "$@"; }
PS() { PC "powershell -NoProfile -EncodedCommand $(printf '%s' "$1" | iconv -t UTF-16LE | base64 -w0)" 2>&1 \
         | { grep -v -E 'CLIXML|<Objs|</Objs>' || true; } | sed '/^$/d'; }
# Only ever stop the installed copy (other softkvm.exe builds, e.g. benchmarks, may be running).
STOP='Get-Process softkvm -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$env:LOCALAPPDATA\Programs\SoftKVM\*" } | Stop-Process -Force' 
TARGET=${PC_SSH##* }                                  # last word: user@host
SCP_ARGS=$(echo "${PC_SSH% *}" | sed -e 's/-p /-P /')  # the rest: options (scp spells port -P)
ARGS=""; [ "${HIDDEN:-0}" = 1 ] && ARGS="--start-hidden"
VERSION=$(cat "$ROOT/VERSION")

if [ "${1:-}" = "--installer" ]; then
  "$ROOT/pc-client/package.sh" >/dev/null
  scp -q $SCP_ARGS "$ROOT/pc-client/dist-installer/SoftKVM-Setup-$VERSION.exe" "$TARGET:SoftKVM-Setup.exe"
  PS 'Start-Process -Wait -FilePath "$env:USERPROFILE\SoftKVM-Setup.exe" -ArgumentList "/S"; Remove-Item "$env:USERPROFILE\SoftKVM-Setup.exe"; "installed"'
else
  "$ROOT/pc-client/build.sh" >/dev/null
  PS "$STOP"'; New-Item -ItemType Directory -Force "$env:LOCALAPPDATA\Programs\SoftKVM" | Out-Null'
  scp -q $SCP_ARGS "$ROOT"/pc-client/dist/* "$TARGET:AppData/Local/Programs/SoftKVM/"
fi

# Start it in the signed-in user's session (SSH commands run in session 0, which has no desktop).
PS "$STOP"'
$exe = "$env:LOCALAPPDATA\Programs\SoftKVM\softkvm.exe"
$a = New-ScheduledTaskAction -Execute $exe -Argument "'"$ARGS"' " -WorkingDirectory (Split-Path $exe)
$p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
Register-ScheduledTask -TaskName "SoftKVM" -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName "SoftKVM"
Start-Sleep -Seconds 5
Get-Content "$env:LOCALAPPDATA\Programs\SoftKVM\softkvm.log" -Tail 8'
