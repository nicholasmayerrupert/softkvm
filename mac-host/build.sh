#!/bin/bash
# Builds SoftKVMHost.app and signs it with the first available code-signing identity,
# so macOS remembers Screen Recording / Accessibility permissions across rebuilds.
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release
APP=SoftKVMHost.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp .build/release/SoftKVMHost "$APP/Contents/MacOS/"
cp Resources/Info.plist "$APP/Contents/"
VERSION=$(cat ../VERSION 2>/dev/null || echo 0.0.0)
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$APP/Contents/Info.plist"
if [ -d ../assets/AppIcon.iconset ]; then
  mkdir -p "$APP/Contents/Resources"
  iconutil -c icns ../assets/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
  /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist"
fi
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(git -C .. rev-list --count HEAD 2>/dev/null || echo 1)" "$APP/Contents/Info.plist"

KC="$HOME/Library/Keychains/softkvm-signing.keychain-db"
if [ -f "$KC" ]; then
  security unlock-keychain -p softkvm "$KC"
  codesign --force --keychain "$KC" --sign "SoftKVM Local Signing" "$APP"
  echo "signed with: SoftKVM Local Signing (run setup-signing.sh to create it)"
else
  echo "warning: no signing keychain; run ./setup-signing.sh once" >&2
  codesign --force --sign - "$APP"
  echo "ad-hoc signed (permissions will reset on every rebuild)"
fi
echo "built $(pwd)/$APP"
