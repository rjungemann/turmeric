#!/usr/bin/env bash
# make-dmg.sh -- wrap a baked Turmeric Studio.app in a distributable DMG.
# macOS only. Unsigned by default; opt in to notarization via
# TURMERIC_NOTARY_PROFILE (a keychain notarytool profile name).

set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: make-dmg.sh <Turmeric Studio.app> [out-dir]" >&2
  exit 1
fi

APP="$1"
OUT_DIR="${2:-dist-out}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo "0.0.0")"
ARCH="$(uname -m | sed 's/x86_64/x86_64/;s/arm64/arm64/')"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "make-dmg.sh: macOS only" >&2
  exit 1
fi
if [ ! -d "$APP" ]; then
  echo "make-dmg.sh: $APP not found" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
DMG="$OUT_DIR/TurmericStudio-$VERSION-macos-$ARCH.dmg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -R "$APP" "$STAGE/"
ln -sf /Applications "$STAGE/Applications"

hdiutil create -volname "Turmeric Studio $VERSION" \
               -srcfolder "$STAGE" \
               -ov -format UDZO "$DMG"

if [ -n "${TURMERIC_NOTARY_PROFILE:-}" ]; then
  echo "submitting for notarization (profile: $TURMERIC_NOTARY_PROFILE)"
  xcrun notarytool submit "$DMG" \
        --keychain-profile "$TURMERIC_NOTARY_PROFILE" \
        --wait
  xcrun stapler staple "$DMG"
fi

echo "produced: $DMG"
