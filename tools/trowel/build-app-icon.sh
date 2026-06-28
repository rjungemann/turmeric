#!/usr/bin/env bash
# Build a macOS .icns from web/public/logo-icon.svg and assemble a
# sibling .app bundle that wears it. The bundle's binary, code resources,
# and data directory are all symlinks back into the user's stock
# Trowel.app or Lite XL.app -- nothing is duplicated, nothing in
# /Applications is modified. The sibling lives at
# tools/trowel/Trowel.app and is meant as a temporary "look, our icon
# in the dock!" affordance ahead of the packaged distribution.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

TROWEL_APP="${TUR_TROWEL_APP:-}"
if [ -z "$TROWEL_APP" ]; then
    if [ -d "/Applications/Trowel.app" ]; then
        TROWEL_APP="/Applications/Trowel.app"
    elif [ -d "/Applications/Lite XL.app" ]; then
        TROWEL_APP="/Applications/Lite XL.app"
    else
        echo "build-app-icon: no base Trowel or Lite XL application bundle found in /Applications" >&2
        echo "  install Trowel or Lite XL first." >&2
        exit 1
    fi
fi

OUT_APP="$ROOT/tools/trowel/Trowel.app"
ICONSET="$ROOT/tools/trowel/trowel.iconset"
ICNS="$ROOT/tools/trowel/trowel.icns"
SRC_SVG="$ROOT/web/public/logo-icon.svg"

if [ ! -f "$SRC_SVG" ]; then
    echo "build-app-icon: $SRC_SVG missing" >&2
    exit 1
fi
command -v magick >/dev/null 2>&1 || {
    echo "build-app-icon: ImageMagick 'magick' not found (brew install imagemagick)" >&2
    exit 1
}

echo "build-app-icon: rasterizing $SRC_SVG -> $ICNS"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
# Render each size square with a transparent background so iconutil
# accepts the input. The SVG is 148x128 (non-square); ImageMagick fits
# it inside the square and centers it via -gravity center -extent.
for s in 16 32 64 128 256 512 1024; do
    magick -background none -density 1024 "$SRC_SVG" \
        -resize "${s}x${s}" \
        -gravity center -extent "${s}x${s}" \
        "$ICONSET/sz$s.png"
done
# iconutil wants Apple-named files in the iconset dir.
mv "$ICONSET/sz16.png"   "$ICONSET/icon_16x16.png"
mv "$ICONSET/sz32.png"   "$ICONSET/icon_16x16@2x.png"
cp "$ICONSET/icon_16x16@2x.png" "$ICONSET/icon_32x32.png"
mv "$ICONSET/sz64.png"   "$ICONSET/icon_32x32@2x.png"
mv "$ICONSET/sz128.png"  "$ICONSET/icon_128x128.png"
mv "$ICONSET/sz256.png"  "$ICONSET/icon_128x128@2x.png"
cp "$ICONSET/icon_128x128@2x.png" "$ICONSET/icon_256x256.png"
mv "$ICONSET/sz512.png"  "$ICONSET/icon_256x256@2x.png"
cp "$ICONSET/icon_256x256@2x.png" "$ICONSET/icon_512x512.png"
mv "$ICONSET/sz1024.png" "$ICONSET/icon_512x512@2x.png"

iconutil -c icns "$ICONSET" -o "$ICNS"
rm -rf "$ICONSET"

echo "build-app-icon: assembling $OUT_APP"
rm -rf "$OUT_APP"
mkdir -p "$OUT_APP/Contents/MacOS" "$OUT_APP/Contents/Resources"
# Binary: real COPY rather than a symlink. macOS resolves binary symlinks
# and reports the underlying bundle to LaunchServices, which means the
# dock icon and process display name would come from the parent app
# instead of ours. Copying the binary is the cheap price of
# a distinct bundle. The whole Trowel.app dir is gitignored as a
# build artifact.
if [ -f "$TROWEL_APP/Contents/MacOS/trowel" ]; then
    BASE_BIN="trowel"
else
    BASE_BIN="lite-xl"
fi
cp "$TROWEL_APP/Contents/MacOS/$BASE_BIN" "$OUT_APP/Contents/MacOS/trowel"
chmod +x "$OUT_APP/Contents/MacOS/trowel"

# Data dirs: symlink every immediate Resources entry except an existing
# icon, which we override with ours.
for entry in "$TROWEL_APP/Contents/Resources/"*; do
    name="$(basename "$entry")"
    case "$name" in
        icon.icns|icon-*.icns) continue ;;
    esac
    ln -s "$entry" "$OUT_APP/Contents/Resources/$name"
done
cp "$ICNS" "$OUT_APP/Contents/Resources/trowel.icns"

# Minimal Info.plist. We keep CFBundleExecutable=trowel because that's
# our binary name; CFBundleIconFile points at our .icns; identifier and
# display name are ours.
cat > "$OUT_APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>           <string>en</string>
  <key>CFBundleDisplayName</key>                 <string>Trowel</string>
  <key>CFBundleExecutable</key>                  <string>trowel</string>
  <key>CFBundleIconFile</key>                    <string>trowel</string>
  <key>CFBundleIdentifier</key>                  <string>com.trowel.editor</string>
  <key>CFBundleInfoDictionaryVersion</key>       <string>6.0</string>
  <key>CFBundleName</key>                        <string>Trowel</string>
  <key>CFBundlePackageType</key>                 <string>APPL</string>
  <key>CFBundleShortVersionString</key>          <string>0.1</string>
  <key>CFBundleVersion</key>                     <string>0.1</string>
  <key>LSMinimumSystemVersion</key>              <string>10.13</string>
  <key>NSHighResolutionCapable</key>             <true/>
  <key>NSPrincipalClass</key>                    <string>NSApplication</string>
</dict>
</plist>
PLIST

# Ad-hoc re-sign so Gatekeeper recognises the modified bundle. (Without
# this, macOS sometimes refuses to launch a cloned .app.)
codesign --force --deep --sign - "$OUT_APP" >/dev/null 2>&1 || true

# Bust LaunchServices caches that pin Finder to the previous icon for
# this bundle identifier. Safe no-op if nothing was cached.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$OUT_APP" >/dev/null 2>&1 || true

echo "build-app-icon: done -> $OUT_APP"
