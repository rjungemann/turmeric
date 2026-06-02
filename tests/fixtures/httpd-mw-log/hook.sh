#!/usr/bin/env bash
# Build + run the mw-log fixture, then normalize the variable elapsed ms
# (e.g. "0ms", "12ms") into a fixed "TIMEms" so the snapshot is stable.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"

CC="$CC" "$TUR" build "$FIXTURE_DIR/input.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe" | sed -E 's/[0-9]+ms$/TIMEms/'
