#!/bin/bash
# Dev/agent deploy: copies this checkout to ~/softkvm on the Mac, builds, installs
# /Applications/SoftKVM Host.app and relaunches it. Needs mac-host/setup-signing.sh run once on the Mac.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/deploy.env"
tar czf - -C "$ROOT" --exclude=.git --exclude=.build --exclude='*.app' --exclude=deps --exclude=dist \
    --exclude=dist-installer --exclude=build --exclude=out --exclude=__pycache__ --exclude=third_party . \
  | ssh $MAC_SSH 'mkdir -p ~/softkvm && cd ~/softkvm && tar xzf -'
ssh $MAC_SSH 'cd ~/softkvm/mac-host && ./install.sh 2>&1 | grep -E "error|installed" ; sleep 2; tail -3 ~/Library/Logs/SoftKVMHost.log'
