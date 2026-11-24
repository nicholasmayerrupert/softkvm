#!/bin/bash
# Cross-compiles softkvm.exe with mingw-w64 and collects it plus DLLs into dist/.
set -euo pipefail
cd "$(dirname "$0")"
[ -d deps ] || ./fetch-deps.sh
SDL=$(ls -d deps/SDL2-*/x86_64-w64-mingw32)
FF=$(ls -d deps/ffmpeg-*-win64-lgpl-shared-* | head -1)
mkdir -p dist
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
  -I"$SDL/include/SDL2" -I"$FF/include" \
  -D_WIN32_WINNT=0x0A00 \
  src/main.c src/keymap.c src/config.c src/discovery.c src/tray.c -o dist/softkvm.exe \
  -L"$SDL/lib" -L"$FF/lib" \
  -lmingw32 -lSDL2 -lavcodec -lavutil -lws2_32 -liphlpapi -lshell32 -ladvapi32 -lgdi32 \
  -static-libgcc -Wl,--subsystem,windows
cp "$SDL/bin/SDL2.dll" dist/
cp "$FF"/bin/avcodec-*.dll "$FF"/bin/avutil-*.dll "$FF"/bin/swresample-*.dll dist/ 2>/dev/null || true
echo "built: $(ls dist)"
