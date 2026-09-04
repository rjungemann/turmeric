#!/usr/bin/env bash
# Phase S7: basic async/await smoke test.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-async-basic: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-async-basic.tur"
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

check_line "await(async(fn => 1+2)) = 3"   "3"
check_line "await(async compute) = 42"      "42"
check_line "await(async(fn => 5+2)) = 7"    "7"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
