#!/usr/bin/env bash
# Self-test for run.sh's emitted-C pointer/integer ratchet.
#
# The ratchet greps a fixture's captured build stderr for -Wint-conversion /
# -Wincompatible-pointer-types.  A grep that silently matches nothing looks
# exactly like a clean corpus, which is the failure mode this guards: while the
# sweep behind docs/archive/emitted-c-pointer-integer-warnings-unwatched.md was
# being written, TWO passes reported zero for reasons that had nothing to do
# with the code -- one had `-w` ahead of `-Wint-conversion` (GCC's -w wins), the
# other syntax-checked the emitted C standalone, where it does not compile.
#
# So: build a program whose emitted C provably mixes a pointer and an integer,
# and assert the pattern fires on it.  If this goes quiet, the ratchet is
# broken, not the corpus clean.
#
# Exits 0 on success, 1 if the canary produced no matching warning.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-cc-warn-ratchet: $TUR not built" >&2; exit 2; }

# The same pattern run.sh matches on.  Kept in sync BY HAND -- if you change one,
# change the other; a drifted pattern is the quiet failure this script exists to
# catch, so it is deliberately spelled out here rather than sourced.
PATTERN='\[-W(int-conversion|incompatible-pointer-types)\]'

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tur-ccwarn.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/canary.tur" <<'TUR_EOF'
(defn f [] : cstr
  ```c
  return 42;
  ```)
(defn main [] : int 0)
TUR_EOF

out=$(CC="${CC:-cc}" "$TUR" build "$tmp/canary.tur" -o "$tmp/canary.bin" 2>&1)

if printf '%s' "$out" | grep -qE "$PATTERN"; then
    echo "cc-warn ratchet OK: canary trips $PATTERN"
    exit 0
fi

echo "check-cc-warn-ratchet: FAILED -- the canary produced no matching warning." >&2
echo "  The ratchet in tests/run.sh cannot catch a pointer/integer confusion in" >&2
echo "  the emitted C, so a clean suite proves nothing about that class." >&2
echo "  Canary build output was:" >&2
printf '%s\n' "$out" | sed 's/^/    /' >&2
exit 1
