#!/usr/bin/env bash
# Phase S7: task composition and chained awaits.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-async-composition: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-async-composition.tur"
PASS=0
FAIL=0

actual=$(printf ':reload %s\n:quit\n' "$FIXTURE" \
    | "$TUR" repl 2>/dev/null \
    | sed 's/\x1b\[[0-9;]*m//g')

check_line() {
    local desc="$1"
    local expected="$2"
    if grep -qF "$expected" <<< "$actual"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected '$expected')"
        echo "  actual output:"
        echo "$actual" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
    fi
}

check_line "two independent tasks (10+20=30)"   "30"
check_line "chained await (50*2=100)"           "100"
check_line "async-all2 (80+120=200)"            "200"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
