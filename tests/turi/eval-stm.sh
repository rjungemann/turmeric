#!/usr/bin/env bash
# Phase TI4 (turi-parity-post-v1-plan): STM under the tree-walking interpreter.
# Runs tests/turi/eval-stm.tur through `tur --interpret` (the real turi/eval.c
# path -- NOT `tur run`, which compiles) and checks the exact stdout.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-stm: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-stm.tur"

# The interpreter intentionally never frees its closures/natives; disable LSan.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

actual=$("$TUR" --interpret "$FIXTURE" 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g')
expected=$(printf 'true\nfalse\ntrue\ntrue\ntrue\ntrue\ntrue')

if [ "$actual" = "$expected" ]; then
    echo "PASS: eval-stm"
    exit 0
fi

echo "FAIL: eval-stm -- output mismatch"
echo "--- expected ---"; printf '%s\n' "$expected"
echo "--- actual ---";   printf '%s\n' "$actual"
exit 1
