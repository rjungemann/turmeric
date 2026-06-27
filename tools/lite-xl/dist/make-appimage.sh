#!/usr/bin/env bash
# make-appimage.sh -- bake the Turmeric plugin into a built Lite XL tree
# and package it as a Linux AppImage.
#
# Usage:
#   bash tools/lite-xl/dist/make-appimage.sh <vendor/lite-xl/build> [out-dir]
#
# Requires `appimagetool` (from https://appimage.github.io) on PATH.

set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: make-appimage.sh <lite-xl-build-dir> [out-dir]" >&2
  exit 1
fi

BUILD="$1"
OUT_DIR="${2:-dist-out}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DIST="$ROOT/tools/lite-xl/dist"
PLUGIN_DIR="$ROOT/tools/lite-xl"
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo "0.0.0")"
ARCH="$(uname -m)"

if [ "$(uname -s)" != "Linux" ]; then
  echo "make-appimage.sh: Linux only" >&2
  exit 1
fi
if ! command -v appimagetool >/dev/null 2>&1; then
  echo "make-appimage.sh: appimagetool not found on PATH" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
APPDIR="$(mktemp -d)/TurmericStudio.AppDir"
trap 'rm -rf "$(dirname "$APPDIR")"' EXIT

mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/lite-xl"

# Copy Lite XL binary + data tree out of the upstream build directory.
# Upstream layout is `lite-xl` (binary) + `data/` (Lua runtime/plugins/colors).
cp "$BUILD/lite-xl" "$APPDIR/usr/bin/lite-xl"
cp -R "$BUILD/data/." "$APPDIR/usr/share/lite-xl/"

mkdir -p "$APPDIR/usr/share/lite-xl/plugins" "$APPDIR/usr/share/lite-xl/colors"
cp "$PLUGIN_DIR/turmeric.lua"               "$APPDIR/usr/share/lite-xl/plugins/turmeric.lua"
cp "$PLUGIN_DIR/colors/turmeric-dark.lua"   "$APPDIR/usr/share/lite-xl/colors/turmeric-dark.lua"
cp "$PLUGIN_DIR/colors/turmeric-light.lua"  "$APPDIR/usr/share/lite-xl/colors/turmeric-light.lua"
cp "$DIST/init.lua"                         "$APPDIR/usr/share/lite-xl/init.lua"

# Desktop entry + icon.
cp "$DIST/turmeric-studio.desktop" "$APPDIR/turmeric-studio.desktop"
if [ -f "$PLUGIN_DIR/turmeric.icns" ]; then
  # AppImage prefers PNG; convert if ImageMagick is present, otherwise warn.
  if command -v magick >/dev/null 2>&1; then
    magick "$PLUGIN_DIR/turmeric.icns[0]" "$APPDIR/turmeric-studio.png"
  else
    echo "warning: ImageMagick missing -- AppImage will lack an icon" >&2
  fi
fi

cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/lite-xl" "$@"
EOF
chmod +x "$APPDIR/AppRun"

OUT="$OUT_DIR/TurmericStudio-$VERSION-linux-$ARCH.AppImage"
ARCH="$ARCH" appimagetool "$APPDIR" "$OUT"
echo "produced: $OUT"
