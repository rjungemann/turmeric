#!/usr/bin/env bash
# tests/run-r7rs-conformance.sh -- r7rs-lang-plan R10: chibi-scheme's R7RS
# test suite (tests/r7rs/chibi-r7rs-tests.scm, BSD, see CHIBI-COPYING), run
# through tests/r7rs/run-conformance.py, which REPORTS A COUNT.
#
# The count is the point: it moves as stages land instead of arriving as a
# verdict at the end.  This target fails only on a REGRESSION -- fewer passes
# than the floor below.  Raise the floor when the count goes up.
#
#   R7RS_CONFORMANCE_BACKEND  both (default) | interp | compiled
#                             The whole suite as one program per round: about
#                             a minute on the interpreter and one C build (a
#                             minute) on the compiled back end.
#   R7RS_CONFORMANCE_FLOOR    override the floor (applied to each back end).
#
# Skips cleanly (exit 0) without python3 or the built binary.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TUR="${TUR:-$ROOT/build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP run-r7rs-conformance: $TUR not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP run-r7rs-conformance: python3 not found"
    exit 0
fi

# The floor: the count recorded in the plan's R10 "What shipped" note, of
# 1216 tests written in the suite -- the same on both back ends.
FLOOR="${R7RS_CONFORMANCE_FLOOR:-1077}"
BACKEND="${R7RS_CONFORMANCE_BACKEND:-both}"

exec python3 tests/r7rs/run-conformance.py --tur "$TUR" --backend "$BACKEND" \
    --min-pass "$FLOOR"
