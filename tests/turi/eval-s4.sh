#!/usr/bin/env bash
# Phase S4 eval test: structs, exceptions, defer

set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-s4: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-s4.tur"

OUTPUT=$(printf ':reload %s\n:quit\n' "$FIXTURE" | "$TUR" repl 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g')

check() {
    local expected="$1"
    if ! grep -qF "$expected" <<< "$OUTPUT"; then
        echo "FAIL: expected '$expected' in output"
        echo "--- output ---"
        echo "$OUTPUT"
        exit 1
    fi
}

check "7"          # test-struct: (.x p) + (.y p) = 3 + 4
check "42"         # test-catch: catch-unwind contained the panic, returned 42
check "defer-ran"  # test-defer: the defer fired during panic unwind
check "99"         # test-defer: catch-unwind contained the panic, returned 99

echo "PASS: eval-s4"
