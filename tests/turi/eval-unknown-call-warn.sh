#!/usr/bin/env bash
# Eval-mode unknown-call diagnostic (TUR-W0040).
#
# In compile mode an unknown call head is a hard error (UCH1). In interpret
# mode the elaborator builds a runtime-dispatch call so late-bound TuriEnv
# natives work without a compile-time declaration -- but a name that is neither
# bound nor in the typed-native registry is almost always a typo. The
# elaborator must emit TUR-W0040 at elaboration time so the diag sink surfaces
# it before the offending line ever runs.
# See docs/archive/history/eval-mode-unknown-call-deferred-to-runtime.md.
set -euo pipefail

cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "eval-unknown-call-warn: $TUR not built" >&2; exit 2; }

FIXTURE="tests/turi/eval-unknown-call-warn.tur"
PASS=0
FAIL=0

# The fixture deliberately calls an unbound name, so --interpret exits non-zero
# (the runtime dispatch fails). We only care about the elaboration-time warning
# on stderr, so swallow the exit status.
actual=$({ "$TUR" --interpret "$FIXTURE" 2>&1 || true; } \
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

check_line "emits TUR-W0040 warning code"          "TUR-W0040"
check_line "warning names the unbound symbol"      "definitely-not-a-real-function"
check_line "warning mentions runtime-dispatch"     "runtime-dispatch"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
