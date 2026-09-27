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
#   tests/r7rs/srfi/<N>/base-except optional: names to leave out of the
#                                   suite's (scheme base), space-separated
#                                   (SRFI 13's string-map and
#                                   string-for-each conflict with R7RS's, D5)
#   tests/r7rs/srfi/<N>/self-hosted optional marker: the suite is an SRFI
#                                   64 program, run whole, and counted by its
#                                   own summary (SRFI 64's meta-suite, whose
#                                   names are the harness's)
#
# Like tur_r7rs_conformance, this fails only on a REGRESSION: fewer passes
# than a suite's floor.  Raise the floor when the count goes up.  Where a suite
# came from and under what licence is in its header.
#
#   R7RS_SRFI_SUITES          the SRFI numbers to run (default: every suite)
#   R7RS_SRFI_SHARD           i/n: of those, in numeric order, only the i-th,
#                             (i+n)-th, ... (1-based); ctest runs two shards
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

SHARD="${R7RS_SRFI_SHARD:-}"
if [ -n "$SHARD" ]; then
    case "$SHARD" in
        */*) shard_i="${SHARD%/*}"; shard_n="${SHARD#*/}" ;;
        *) echo "FAIL run-r7rs-srfi-suites: R7RS_SRFI_SHARD='$SHARD' is not i/n"; exit 1 ;;
    esac
    k=0
    picked=""
    for n in $NUMS; do
        if [ $((k % shard_n + 1)) -eq "$shard_i" ]; then picked="$picked $n"; fi
        k=$((k + 1))
    done
    NUMS="$picked"
    echo "run-r7rs-srfi-suites: shard $SHARD:$NUMS"
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
    if [ -f "$dir/self-hosted" ]; then
        extra+=(--self-hosted)
    fi
    if [ -f "$dir/base-except" ]; then
        for name in $(cat "$dir/base-except"); do
            extra+=(--base-except "$name")
        done
    fi
    python3 tests/r7rs/run-conformance.py --tur "$TUR" --backend "$BACKEND" \
        --suite "$dir/tests.scm" --import "(srfi $n)" ${extra[@]+"${extra[@]}"} --label "r7rs-srfi-$n" \
        --min-pass "$floor" --list-failures || status=1
done
exit $status
