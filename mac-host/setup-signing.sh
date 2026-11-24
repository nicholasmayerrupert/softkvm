#!/bin/bash
# One-time: creates a self-signed code-signing identity in a dedicated keychain so
# build.sh can sign headlessly (e.g. over SSH, where the login keychain is locked).
# A stable signature lets macOS remember Screen Recording / Accessibility grants.
set -euo pipefail
KC="$HOME/Library/Keychains/softkvm-signing.keychain-db"
PASS="softkvm"   # protects only this throwaway signing key
NAME="SoftKVM Local Signing"

if [ -f "$KC" ]; then echo "already set up: $KC"; exit 0; fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=$NAME" \
  -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
  -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=critical,codeSigning" 2>/dev/null
openssl pkcs12 -export -legacy -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -out "$TMP/id.p12" -passout pass:$PASS 2>/dev/null \
  || openssl pkcs12 -export -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -out "$TMP/id.p12" -passout pass:$PASS

security create-keychain -p "$PASS" "$KC"
security set-keychain-settings "$KC"   # no auto-lock
security unlock-keychain -p "$PASS" "$KC"
security import "$TMP/id.p12" -k "$KC" -P "$PASS" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$PASS" "$KC" >/dev/null
echo "created $KC with identity '$NAME'"
