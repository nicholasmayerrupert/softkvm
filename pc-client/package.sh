#!/bin/bash
# Builds softkvm.exe and wraps it in a per-user installer: dist-installer/SoftKVM-Setup-<version>.exe
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
VERSION=$(cat ../VERSION)
mkdir -p dist-installer
makensis -V2 -DVERSION="$VERSION" -DDIST="$(pwd)/dist" -DICON="$(pwd)/../assets/softkvm.ico" \
  -DOUTFILE="$(pwd)/dist-installer/SoftKVM-Setup-$VERSION.exe" installer/softkvm.nsi
echo "installer: $(pwd)/dist-installer/SoftKVM-Setup-$VERSION.exe"
