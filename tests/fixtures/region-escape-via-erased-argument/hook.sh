#!/usr/bin/env bash
# Build prog.tur, run it with TUR_REGION_STATS=1, and print the program's
# stdout followed by the region-stats line, so expected.stdout pins both the
# values and the rewind/retire counts.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
CC="$CC" "$TUR" build "$FIXTURE_DIR/prog.tur" -o "$TMPDIR/prog" > "$TMPDIR/build.log" 2>&1 || {
    cat "$TMPDIR/build.log" >&2
    exit 1
}
TUR_REGION_STATS=1 ASAN_OPTIONS=detect_leaks=0 "$TMPDIR/prog" 2> "$TMPDIR/stats"
grep '^region-stats:' "$TMPDIR/stats"
