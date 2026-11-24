#!/bin/bash
# Builds the host and installs it as /Applications/SoftKVM Host.app, replacing any running copy.
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
DEST="/Applications/SoftKVM Host.app"
pkill -x SoftKVMHost 2>/dev/null && sleep 1 || true
rm -rf "$DEST"
ditto SoftKVMHost.app "$DEST"
rm -rf SoftKVMHost.app   # keep a single copy so Launch Services / Login Items point at /Applications
open "$DEST"
echo "installed $DEST"
