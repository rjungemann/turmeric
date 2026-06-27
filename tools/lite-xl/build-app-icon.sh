#!/usr/bin/env bash
# Build a macOS .icns from web/public/logo-icon.svg and assemble a
# sibling .app bundle that wears it. The bundle's binary, code resources,
# and data directory are all symlinks back into the user's stock
# /Applications/Lite XL.app -- nothing is duplicated, nothing in
# /Applications is modified. The sibling lives at
# tools/lite-xl/Turmeric.app and is meant as a temporary "look, our icon
# in the dock!" affordance ahead of the Phase 5 bundled distribution.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LITE_XL_APP="${TUR_LITE_XL_APP:-/Applications/Lite XL.app}"
OUT_APP="$ROOT/tools/lite-xl/Turmeric.app"
ICONSET="$ROOT/tools/lite-xl/turmeric.iconset"
ICNS="$ROOT/tools/lite-xl/turmeric.icns"
SRC_SVG="$ROOT/web/public/logo-icon.svg"

if [ ! -d "$LITE_XL_APP" ]; then
    echo "build-app-icon: $LITE_XL_APP missing" >&2
    echo "  install Lite XL first (https://lite-xl.com)" >&2
    exit 1
fi
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
# dock icon and process display name would come from /Applications/Lite
# XL.app instead of ours. Copying the ~3 MB binary is the cheap price of
# a distinct bundle. The whole Turmeric.app dir is gitignored as a
# build artifact.
cp "$LITE_XL_APP/Contents/MacOS/lite-xl" "$OUT_APP/Contents/MacOS/lite-xl"
chmod +x "$OUT_APP/Contents/MacOS/lite-xl"
# Data dirs: symlink every immediate Resources entry except an existing
# icon, which we override with ours.
for entry in "$LITE_XL_APP/Contents/Resources/"*; do
    name="$(basename "$entry")"
    case "$name" in
        icon.icns|icon-*.icns) continue ;;
    esac
    ln -s "$entry" "$OUT_APP/Contents/Resources/$name"
done
cp "$ICNS" "$OUT_APP/Contents/Resources/turmeric.icns"

# Minimal Info.plist. We keep CFBundleExecutable=lite-xl because that's
# the binary name; CFBundleIconFile points at our .icns; identifier and
# display name are ours.
cat > "$OUT_APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>           <string>en</string>
  <key>CFBundleDisplayName</key>                 <string>Turmeric</string>
  <key>CFBundleExecutable</key>                  <string>lite-xl</string>
  <key>CFBundleIconFile</key>                    <string>turmeric</string>
  <key>CFBundleIdentifier</key>                  <string>dev.turmeric.editor</string>
  <key>CFBundleInfoDictionaryVersion</key>       <string>6.0</string>
  <key>CFBundleName</key>                        <string>Turmeric</string>
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
