#!/bin/bash
# Builds the host and wraps it in a disk image: dist/SoftKVM-Host-<version>.dmg
# (drag "SoftKVM Host" onto Applications).
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
VERSION=$(cat ../VERSION)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ditto SoftKVMHost.app "$STAGE/SoftKVM Host.app"
ln -s /Applications "$STAGE/Applications"
mkdir -p dist
rm -f "dist/SoftKVM-Host-$VERSION.dmg"
hdiutil create -quiet -volname "SoftKVM Host $VERSION" -srcfolder "$STAGE" -ov -format UDZO \
  "dist/SoftKVM-Host-$VERSION.dmg"
echo "disk image: $(pwd)/dist/SoftKVM-Host-$VERSION.dmg"
