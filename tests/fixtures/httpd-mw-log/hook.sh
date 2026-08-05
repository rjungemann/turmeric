#!/usr/bin/env bash
# Build + run the mw-log fixture, then normalize the variable elapsed ms
# (e.g. "0ms", "12ms") into a fixed "TIMEms" so the snapshot is stable.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"

# Honor the sibling `flags` file (e.g. --enable=closure-drop-glue) so the hook
# build matches how the regular runner would build this fixture. Without it the
# hook would build flag-off and, with leak detection on (no requires.no-leak-check
# marker), the flag-off inner-`next`-box leak would abort via LSan _exit() and
# drop the buffered final line.
FLAGS=""
[ -f "$FIXTURE_DIR/flags" ] && FLAGS="$(cat "$FIXTURE_DIR/flags")"
CC="$CC" "$TUR" $FLAGS build "$FIXTURE_DIR/input.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe" | sed -E 's/[0-9]+ms$/TIMEms/'
