#!/bin/sh
# fetch-sdl2.sh — download the SDL2 MinGW devel release into external/SDL2/
#
# POSIX sh only (no bashisms) so it runs under w64devkit's busybox ash as
# well as real bash.
#
# This is only needed for LOCAL Windows builds (e.g. with w64devkit).  In
# CI and on Linux / macOS the build uses pkg-config / sdl2-config to find
# a system SDL2, so this script is not needed there.
#
# Must be run from the repo root:
#   make fetch-sdl2                      # default version
#   SDL2_VERSION=2.30.11 make fetch-sdl2 # specific version

set -eu

VERSION="${SDL2_VERSION:-2.30.11}"
URL="https://github.com/libsdl-org/SDL/releases/download/release-${VERSION}/SDL2-devel-${VERSION}-mingw.zip"

# Verify we're in the repo root.
if [ ! -d sim ] || [ ! -d m68k ]; then
    echo "Error: run this via 'make fetch-sdl2' from the repository root." >&2
    exit 1
fi

EXTERNAL="external"
TARGET="$EXTERNAL/SDL2"
ZIP="$EXTERNAL/SDL2-devel-${VERSION}-mingw.zip"

mkdir -p "$EXTERNAL"

if [ -d "$TARGET" ]; then
    echo "external/SDL2 already exists."
    echo "Delete it first if you want to re-fetch:  rm -rf external/SDL2"
    exit 0
fi

echo "Downloading SDL2 ${VERSION} MinGW devel from:"
echo "  $URL"

if command -v curl >/dev/null 2>&1; then
    curl -L --fail -o "$ZIP" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$ZIP" "$URL"
else
    echo "Error: neither curl nor wget is available." >&2
    exit 1
fi

echo "Extracting..."
(cd "$EXTERNAL" && unzip -q "SDL2-devel-${VERSION}-mingw.zip")
mv "$EXTERNAL/SDL2-${VERSION}" "$TARGET"
rm "$ZIP"

echo
echo "SDL2 ${VERSION} ready at: $TARGET"
ls "$TARGET" 2>/dev/null || true
