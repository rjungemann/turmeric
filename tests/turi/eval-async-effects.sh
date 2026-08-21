#!/usr/bin/env bash
# Phase S7: algebraic effects across async/await boundaries.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-async-effects: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-async-effects.tur"
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

check_line "println before await"        "before-await"
check_line "println after await"         "after-await"
check_line "async body result (42)"      "42"
check_line "effect inside async body"    "hello"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
