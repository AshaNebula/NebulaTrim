#!/usr/bin/env bash
# Compila NebulaTrim para Windows (x64) desde Linux y genera un .zip listo para usar.
#
# La primera vez descarga (en .cache/windows/) el compilador llvm-mingw y las versiones
# para Windows de SDL3, libmpv y FFmpeg (~210 MB). Requiere: cmake, curl, bsdtar (libarchive).
#
#   ./scripts/build-windows.sh
#   -> dist/NebulaTrim-windows-x64/  y  dist/NebulaTrim-windows-x64.zip
#
# Para usar otras versiones, cambia las URL de abajo.
set -euo pipefail

LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64.tar.xz"
SDL3_URL="https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-mingw.tar.gz"
MPV_URL="https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261111/mpv-dev-x86_64-20261111-git-bcde6b93cc.7z"
FFMPEG_URL="https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${NEBULATRIM_DEPS:-$ROOT/.cache/windows}"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist/NebulaTrim-windows-x64"

for tool in cmake curl bsdtar; do
    command -v "$tool" >/dev/null || { echo "Falta '$tool'. Instálalo e inténtalo de nuevo." >&2; exit 1; }
done

# fetch <url> <carpeta>: descarga y extrae una sola vez
fetch() {
    local url=$1 dir=$CACHE/$2 file
    file=$CACHE/$(basename "$url")
    [ -f "$dir/.ok" ] && return 0
    mkdir -p "$CACHE"
    if [ ! -f "$file" ]; then
        echo "Descargando $(basename "$url")..."
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    bsdtar -xf "$file" -C "$dir"
    touch "$dir/.ok"
}
fetch "$LLVM_MINGW_URL" llvm-mingw
fetch "$SDL3_URL" sdl3
fetch "$MPV_URL" mpv
fetch "$FFMPEG_URL" ffmpeg

LLVM=$(find "$CACHE/llvm-mingw" -mindepth 1 -maxdepth 1 -type d | head -n1)
SDL=$(find "$CACHE/sdl3" -maxdepth 2 -type d -name x86_64-w64-mingw32 | head -n1)
MPV="$CACHE/mpv"
FFBIN=$(dirname "$(find "$CACHE/ffmpeg" -maxdepth 3 -type f -name ffmpeg.exe | head -n1)")
[ -n "$LLVM" ] && [ -n "$SDL" ] && [ -f "$MPV/libmpv-2.dll" ] && [ -f "$FFBIN/ffmpeg.exe" ] || {
    echo "Las dependencias descargadas no tienen la estructura esperada (borra $CACHE y reintenta)." >&2
    exit 1
}

rm -rf "$BUILD"
MINGW_ROOT="$LLVM" cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw64.cmake" \
    -DSDL3_DIR="$SDL/lib/cmake/SDL3" -DMPV_DIR="$MPV"
cmake --build "$BUILD" -j"$(nproc)"

rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/nebulatrim.exe" "$SDL/bin/SDL3.dll" "$MPV/libmpv-2.dll" "$DIST/"
cp "$FFBIN/ffmpeg.exe" "$FFBIN/ffprobe.exe" "$FFBIN"/*.dll "$DIST/"
cp "$ROOT/third_party/imgui/LICENSE.txt" "$DIST/LICENSE-imgui.txt"
cp "$ROOT/LICENSE" "$DIST/LICENSE"
cat > "$DIST/THIRD-PARTY-NOTICES.txt" <<NOTICE
NebulaTrim is licensed under the GNU GPL v3 (see LICENSE).
Source code: see the NebulaTrim repository.

This package bundles third-party software:
- SDL3 (zlib license) - https://libsdl.org
- Dear ImGui (MIT, see LICENSE-imgui.txt) - https://github.com/ocornut/imgui
- libmpv (GPLv2+) - build and source: $MPV_URL
- FFmpeg (GPL build, see LICENSE-ffmpeg.txt) - build and source: $FFMPEG_URL
NOTICE
FFLICENSE=$(find "$CACHE/ffmpeg" -maxdepth 2 -name 'LICENSE*' | head -n1)
[ -n "$FFLICENSE" ] && cp "$FFLICENSE" "$DIST/LICENSE-ffmpeg.txt"

(cd "$DIST/.." && rm -f NebulaTrim-windows-x64.zip && bsdtar -a -cf NebulaTrim-windows-x64.zip NebulaTrim-windows-x64)
echo
echo "Listo:"
echo "  $DIST ($(du -sh "$DIST" | cut -f1))"
echo "  $ROOT/dist/NebulaTrim-windows-x64.zip ($(du -sh "$ROOT/dist/NebulaTrim-windows-x64.zip" | cut -f1))"
