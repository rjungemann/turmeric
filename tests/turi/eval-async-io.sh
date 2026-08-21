#!/usr/bin/env bash
# Phase S7: non-blocking I/O round-trip (read-async / write-async).
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-async-io: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-async-io.tur"
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

check_line "write-async returns byte count (5)"  "5"
check_line "read-async returns written data"      "hello"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
