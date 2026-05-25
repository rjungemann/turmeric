#!/usr/bin/env bash
# tests/turi/eval-test.sh — Phase S0 REPL smoke test
#
# Pipes scripted input ok? `tur repl` and verifies expected output.

set -u
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-test: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0

# Run a REPL session with the given input and check that output contains
# the expected string.
check() {
    local desc="$1"
    local input="$2"
    local expected="$3"

    actual=$(printf '%s\n:quit\n' "$input" | "$TUR" repl 2>/dev/null)
    if echo "$actual" | grep -qF "$expected"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  input:    $input"
        echo "  expected: $expected"
        echo "  actual:   $actual"
        FAIL=$((FAIL + 1))
    fi
}

# Basic arithmetic
check "(+ 1 2) => 3"       "(+ 1 2)"       "=> 3"
check "(* 6 7) => 42"      "(* 6 7)"       "=> 42"
check "(- 10 3) => 7"      "(- 10 3)"      "=> 7"
check "(/ 20 4) => 5"      "(/ 20 4)"      "=> 5"

# Comparison
check "(< 1 2) => true"    "(< 1 2)"       "=> true"
check "(= 4 4) => true"    "(= 4 4)"       "=> true"
check "(> 3 5) => false"   "(> 3 5)"       "=> false"

# Let
check "let"                "(let [x 10] x)"  "=> 10"

# If
check "if true"            "(if true 1 2)"   "=> 1"
check "if false"           "(if false 1 2)"  "=> 2"

# Defn + call (multi-line via separate check calls)
defn_result=$(printf '(defn sq [x :int] :int (* x x))\n(sq 7)\n:quit\n' \
    | "$TUR" repl 2>/dev/null)
if echo "$defn_result" | grep -qF "=> 49"; then
    echo "PASS: defn + call"
    PASS=$((PASS + 1))
else
    echo "FAIL: defn + call"
    echo "  actual: $defn_result"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
