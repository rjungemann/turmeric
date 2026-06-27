#!/usr/bin/env bash
# vendor.sh -- clone upstream Lite XL into vendor/lite-xl/ at a pinned tag.
# Idempotent: re-runs check out the pinned ref.

set -euo pipefail

PIN="${LITE_XL_PIN:-v2.1.8}"
REPO="${LITE_XL_REPO:-https://github.com/lite-xl/lite-xl}"

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DEST="$ROOT/vendor/lite-xl"

if [ ! -d "$DEST/.git" ]; then
  mkdir -p "$(dirname "$DEST")"
  git clone --depth 1 --branch "$PIN" "$REPO" "$DEST"
else
  ( cd "$DEST" && git fetch --depth 1 origin "$PIN" && git checkout --detach "$PIN" )
fi

echo "vendored Lite XL $PIN at $DEST"
