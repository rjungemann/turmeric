#!/usr/bin/env bash
# Phase S7: non-blocking I/O round-trip (read-async / write-async).
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-async-io: $TUR not built" >&2; exit 2; }

# read-async hands its result string to turi_cstr, which BORROWS the pointer --
# the tree-walking interpreter never frees, by design (CLAUDE.md).  run-turi.sh
# exports detect_leaks=0 for the whole suite; without the same opt-out here a
# standalone run dies before its first assertion, with no output at all.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

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
