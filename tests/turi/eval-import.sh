#!/usr/bin/env bash
# tests/turi/eval-import.sh — Phase S2: (import turi/eval) test
#
# Compiles tests/fixtures/eval-import/input.tur with tur build, then runs
# the binary and checks output.  The fixture uses (import turi/eval) which
# triggers the stdlib fallback search (stdlib/turi/eval.tur) and the
# __tur_auok?link__ mechanism ok? link -lturi.
#
# TUR_CC_FLAGS must include the right -I and -L paths for libturi.
# When run from the project root (e.g. via CTest), set:
#   TUR_CC_FLAGS="-O2 -std=c99 -Wall -I./src -L./build/src"
# The -lturi flag is injected auok?matically by cmd_build from the generated C.

set -u
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-import: $TUR not built" >&2; exit 2; }

FIXTURE="tests/fixtures/eval-import/input.tur"
EXPECTED="tests/fixtures/eval-import/expected.stdout"

PASS=0
FAIL=0

check() {
    local desc="$1"
    local actual="$2"
    local expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected: $(printf '%q' "$expected")"
        echo "  actual:   $(printf '%q' "$actual")"
        FAIL=$((FAIL + 1))
    fi
}

# Build the fixture
TMPEXE=$(mktemp /tmp/tur-eval-import-XXXXXX)
trap 'rm -f "$TMPEXE"' EXIT

build_stderr=$(CC="${CC:-cc}" "$TUR" build "$FIXTURE" -o "$TMPEXE" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    echo "FAIL: build failed (exit $build_rc)"
    echo "  stderr: $build_stderr"
    FAIL=$((FAIL + 1))
    echo ""
    echo "0 passed, $FAIL failed"
    exit 1
fi

# Run and compare
actual_out=$("$TMPEXE" 2>/dev/null)
expected_out=$(cat "$EXPECTED")

check "eval-import output" "$actual_out" "${expected_out%$'\n'}"

echo ""
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
