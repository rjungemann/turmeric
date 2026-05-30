#!/usr/bin/env bash
# tests/run-leak-gate.sh -- LeakSanitizer regression gate for composite-type
# diagnostics (PH3.2).
#
# tests/run.sh compiles the *generated program* without ASan and only the `tur`
# binary itself is sanitized, so a leak in `tur`'s own error-reporting path can
# slip past the normal suite (no committed happy fixture exercises a
# composite-type error). This gate runs the sanitized compiler directly on
# handler-mismatch error fixtures with LeakSanitizer ON and asserts:
#
#   1. the expected diagnostic fires (we are genuinely on the error path), and
#   2. LeakSanitizer reports no leak.
#
# A reintroduced `type_name` leak on a composite-type diagnostic (see
# docs/handler-typecheck-and-typename-followups-plan.md PH2) makes (2) fail.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

# This gate's whole point is to detect leaks, so force LeakSanitizer ON
# regardless of the repo-wide detect_leaks=0 default used by interpreter
# harnesses. The compiler/check path is leak-clean by policy (see CLAUDE.md),
# so a leak here is a real regression.
export ASAN_OPTIONS="detect_leaks=1:exitcode=23"

TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL leak-gate -- tur binary not found at $TUR (build first)"
    exit 1
fi

# Each case: <fixture-dir> <expected-diag-substring>
check_leak_clean() {
    local name="$1" input="$2" needle="$3"
    local out
    out=$(ASAN_OPTIONS="$ASAN_OPTIONS" "$TUR" -Xeffect-types check "$input" 2>&1)

    # Sanity: we must actually be exercising the diagnostic path.
    if ! printf '%s' "$out" | grep -F -q "$needle"; then
        fail "$name" "expected diagnostic '$needle' not emitted -- not on the error path"
        return
    fi
    # The real assertion: no LeakSanitizer report.
    if printf '%s' "$out" | grep -q "LeakSanitizer: detected memory leaks"; then
        fail "$name" "LeakSanitizer reported a leak on a composite-type diagnostic"
        printf '%s\n' "$out" | sed 's/^/    /'
        return
    fi
    pass "$name"
}

check_leak_clean "leak-gate/handler-wrong-effect-set" \
    "tests/fixtures/errors/handler-arg-wrong-effect-set/input.tur" \
    "TUR-E0001"

check_leak_clean "leak-gate/handler-wrong-value-kind" \
    "tests/fixtures/errors/handler-arg-wrong-value-kind/input.tur" \
    "TUR-E0001"

echo "leak-gate summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
