#!/usr/bin/env bash
# bake-bundle.sh -- overlay the Turmeric plugin / themes / icon / Info.plist
# onto an already-built upstream `lite-xl.app` to produce
# `Turmeric Studio.app`. macOS only.
#
# Usage:
#   bash tools/lite-xl/dist/bake-bundle.sh <path/to/lite-xl.app> [out-dir]
#
# Signing is opt-in via TURMERIC_SIGN_IDENTITY (a Developer ID Application
# identity in the keychain). Unsigned bundles are fine for local install /
# nightly testing but will be Gatekeeper-quarantined when downloaded.

set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: bake-bundle.sh <lite-xl.app> [out-dir]" >&2
  exit 1
fi

SRC_APP="$1"
OUT_DIR="${2:-dist-out}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DIST="$ROOT/tools/lite-xl/dist"
PLUGIN_DIR="$ROOT/tools/lite-xl"
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo "0.0.0")"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "bake-bundle.sh: macOS only" >&2
  exit 1
fi
if [ ! -d "$SRC_APP" ]; then
  echo "bake-bundle.sh: $SRC_APP not found" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
DST_APP="$OUT_DIR/Turmeric Studio.app"
rm -rf "$DST_APP"
cp -R "$SRC_APP" "$DST_APP"

RES="$DST_APP/Contents/Resources"
mkdir -p "$RES/plugins" "$RES/colors"

# Plugin + themes + bundled init.lua.
cp "$PLUGIN_DIR/turmeric.lua"               "$RES/plugins/turmeric.lua"
cp "$PLUGIN_DIR/colors/turmeric-dark.lua"   "$RES/colors/turmeric-dark.lua"
cp "$PLUGIN_DIR/colors/turmeric-light.lua"  "$RES/colors/turmeric-light.lua"
cp "$DIST/init.lua"                         "$RES/init.lua"

# Icon (built by tools/lite-xl/build-app-icon.sh if absent).
ICON_SRC="$PLUGIN_DIR/turmeric.icns"
if [ -f "$ICON_SRC" ]; then
  cp "$ICON_SRC" "$RES/turmeric.icns"
else
  echo "bake-bundle.sh: warning -- $ICON_SRC missing (run build-app-icon.sh first)" >&2
fi

# Info.plist with version substituted.
sed "s/@TURMERIC_VERSION@/$VERSION/g" "$DIST/Info.plist" > "$DST_APP/Contents/Info.plist"

# Optional code-signing.
if [ -n "${TURMERIC_SIGN_IDENTITY:-}" ]; then
  echo "signing with: $TURMERIC_SIGN_IDENTITY"
  codesign --force --deep --options runtime --sign "$TURMERIC_SIGN_IDENTITY" "$DST_APP"
fi

echo "baked: $DST_APP"
