#!/usr/bin/env bash
# tests/turi/eval-effects.sh — Phase S3: algebraic effects via the eval core.
#
# Loads eval-effects.tur into the REPL via :reload and verifies that the
# four println calls produce the expected output.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-effects: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-effects.tur"

PASS=0
FAIL=0

# Capture output of :reload (strips ANSI codes)
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

check_line "basic perform/resume (42)"          "42"
check_line "multiple performs (104)"            "104"
check_line "handler without resume (99)"        "99"
check_line "nested handles (52)"                "52"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
