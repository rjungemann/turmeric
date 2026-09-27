#!/usr/bin/env bash
# tests/run-r7rs-srfi-suites.sh -- r7rs-srfi-plan D7: each SRFI's own test
# suite, where one exists, run through tests/r7rs/run-conformance.py on both
# back ends, which REPORTS A COUNT per SRFI.
#
#   tests/r7rs/srfi/<N>/tests.scm   the suite as top-level forms, written in
#                                   (chibi test)'s vocabulary, (srfi N) imported
#   tests/r7rs/srfi/<N>/floor       the pass count it must not fall below
#   tests/r7rs/srfi/<N>/imports     optional: more import sets the suite
#                                   needs, one per line (SRFI 69's uses
#                                   SRFI 1's lset=)
#
# Like tur_r7rs_conformance, this fails only on a REGRESSION: fewer passes
# than a suite's floor.  Raise the floor when the count goes up.  Where a suite
# came from and under what licence is in its header.
#
#   R7RS_SRFI_SUITES          the SRFI numbers to run (default: every suite)
#   R7RS_CONFORMANCE_BACKEND  both (default) | interp | compiled
#
# Skips cleanly (exit 0) without python3 or the built binary.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TUR="${TUR:-$ROOT/build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP run-r7rs-srfi-suites: $TUR not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP run-r7rs-srfi-suites: python3 not found"
    exit 0
fi

BACKEND="${R7RS_CONFORMANCE_BACKEND:-both}"
if [ -n "${R7RS_SRFI_SUITES:-}" ]; then
    NUMS="$R7RS_SRFI_SUITES"
else
    NUMS=$(ls tests/r7rs/srfi | sort -n)
fi

status=0
for n in $NUMS; do
    dir="tests/r7rs/srfi/$n"
    if [ ! -f "$dir/tests.scm" ] || [ ! -f "$dir/floor" ]; then
        echo "FAIL run-r7rs-srfi-suites: $dir needs tests.scm and floor"
        status=1
        continue
    fi
    floor=$(tr -d '[:space:]' < "$dir/floor")
    extra=()
    if [ -f "$dir/imports" ]; then
        while IFS= read -r set; do
            [ -n "$set" ] && extra+=(--import "$set")
        done < "$dir/imports"
    fi
    python3 tests/r7rs/run-conformance.py --tur "$TUR" --backend "$BACKEND" \
        --suite "$dir/tests.scm" --import "(srfi $n)" ${extra[@]+"${extra[@]}"} --label "r7rs-srfi-$n" \
        --min-pass "$floor" --list-failures || status=1
done
exit $status
