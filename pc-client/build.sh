#!/bin/bash
# Cross-compiles softkvm.exe with mingw-w64 and collects it plus DLLs into dist/.
set -euo pipefail
cd "$(dirname "$0")"
[ -d deps ] || ./fetch-deps.sh
SDL=$(ls -d deps/SDL2-*/x86_64-w64-mingw32)
FF=$(ls -d deps/ffmpeg-*-win64-lgpl-shared-* | head -1)
VERSION=$(cat ../VERSION)
IFS=. read -r V1 V2 V3 <<< "$VERSION"
mkdir -p dist build
# Icon, version info and manifest resource
sed -e "s|@ICON@|$(pwd)/../assets/softkvm.ico|" -e "s|@MANIFEST@|$(pwd)/installer/softkvm.manifest|" \
    -e "s|@VERSION@|$VERSION|g" -e "s|@V1@|${V1:-0}|g" -e "s|@V2@|${V2:-0}|g" -e "s|@V3@|${V3:-0}|g" \
    installer/softkvm.rc.in > build/softkvm.rc
x86_64-w64-mingw32-windres build/softkvm.rc -O coff -o build/softkvm.res
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
  -I"$SDL/include/SDL2" -I"$FF/include" \
  -D_WIN32_WINNT=0x0A00 -DSOFTKVM_VERSION="\"$VERSION\"" \
  src/main.c src/keymap.c src/config.c src/discovery.c src/tray.c src/secure.c src/perf.c src/d3d_present.c \
  third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c build/softkvm.res \
  -o dist/softkvm.exe \
  -L"$SDL/lib" -L"$FF/lib" \
  -lmingw32 -lSDL2 -lavcodec -lavutil -lws2_32 -liphlpapi -lshell32 -ladvapi32 -lgdi32 -lbcrypt -lcrypt32 \
  -ld3d11 -ldxgi -luuid \
  -static-libgcc -Wl,--subsystem,windows
cp "$SDL/bin/SDL2.dll" dist/
cp "$FF"/bin/avcodec-*.dll "$FF"/bin/avutil-*.dll "$FF"/bin/swresample-*.dll dist/ 2>/dev/null || true
echo "built: $(ls dist)"
