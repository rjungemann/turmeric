#!/usr/bin/env bash
# Phase TCO: tail call optimization smoke tests.
# Verifies that deeply recursive tail calls do not overflow the C stack.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-tco: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-tco.tur"
PASS=0
FAIL=0

actual=$(printf ':reload %s\n:quit\n' "$FIXTURE" \
    | "$TUR" repl 2>/dev/null \
    | sed 's/\x1b\[[0-9;]*m//g')

check_line() {
    local desc="$1"
    local expected="$2"
    if echo "$actual" | grep -qF "$expected"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected '$expected')"
        echo "  actual output:"
        echo "$actual" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
    fi
}

check_line "count-down 100000 = 0"        "0"
check_line "sum-tc 1..100000 = 5000050000" "5000050000"
check_line "even? 1000 = true (done)"      "done"
check_line "fact-tc 5 1 = 120"             "120"
check_line "count-zero 100000 = 0"         "0"
check_line "fld-sum 99999 = 4999950000 (SR N2 get-field heap-bounded)" "4999950000"
check_line "asc-sum 80000 = 3200040000 (SR N3 ascription heap-bounded)" "3200040000"
check_line "ret-sum 60000 = 1800030000 (SR N3 return heap-bounded)" "1800030000"
check_line "shift-thunk 50000 = 50000 (SR N3b abortive shift driven)" "50000"
check_line "nest-sh 200000 = 0 (SR N4 nested reset/shift heap-bounded)" "0"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
