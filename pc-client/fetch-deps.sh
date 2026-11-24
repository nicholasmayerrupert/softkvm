#!/bin/bash
# Downloads SDL2 (mingw dev) and FFmpeg (BtbN win64 LGPL shared build) into deps/.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p deps && cd deps
SDL_VER=2.32.10
if [ ! -d "SDL2-$SDL_VER" ]; then
  curl -sSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz" | tar xz
fi
if ! ls -d ffmpeg-*-win64-lgpl-shared-* >/dev/null 2>&1; then
  URL=$(curl -sSL https://api.github.com/repos/BtbN/FFmpeg-Builds/releases/latest \
        | grep -o 'https://[^"]*win64-lgpl-shared-[0-9.]*\.zip' | sort -V | tail -1)
  curl -sSL -o ff.zip "$URL"
  python3 -c "import zipfile; zipfile.ZipFile('ff.zip').extractall('.')"
  rm ff.zip
fi
ls
