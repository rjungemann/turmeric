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

# LeakSanitizer is Linux-only: on macOS (and other platforms without LSan)
# AddressSanitizer aborts at startup with "detect_leaks is not supported on
# this platform" before `tur` runs, so the gate cannot execute there.  Probe
# once and skip cleanly rather than reporting spurious failures.  (Detected at
# runtime via the diagnostic re-run added in run-leak-gate; see commit history.)
probe=$(ASAN_OPTIONS="detect_leaks=1:exitcode=23" "$TUR" --version 2>&1)
if grep -q "detect_leaks is not supported" <<< "$probe"; then
    echo "PASS leak-gate (skipped: LeakSanitizer unsupported on this platform)"
    echo "leak-gate summary: skipped -- no LSan on this platform"
    exit 0
fi

# Each case: <fixture-dir> <expected-diag-substring>
check_leak_clean() {
    local name="$1" input="$2" needle="$3"
    local out rc
    out=$(ASAN_OPTIONS="$ASAN_OPTIONS" "$TUR" -Xeffect-types check "$input" 2>&1)
    rc=$?

    # Sanity: we must actually be exercising the diagnostic path.
    if ! printf '%s' "$out" | grep -F -q "$needle"; then
        fail "$name" "expected diagnostic '$needle' not emitted -- not on the error path"
        # Divergence diagnostics (this has reproduced on macOS CI but not on
        # Linux).  Surface enough to tell apart the two candidate causes:
        #   1. the type-checker genuinely accepts the program on this platform
        #      (a real logic divergence), or
        #   2. the leak-check exit (exitcode=23 -> _exit) truncates buffered
        #      stdio before the diagnostic is flushed (the diag was emitted but
        #      lost from the captured stream).
        # The re-run with detect_leaks=0 discriminates: if the needle reappears
        # there, it is (2) -- an output-flush/exit interaction, not a typecheck
        # divergence; if still absent, it is (1).
        echo "    exit code : $rc   (ASAN_OPTIONS=$ASAN_OPTIONS)"
        echo "    output    : ${#out} bytes captured (stdout+stderr)"
        if grep -q "LeakSanitizer" <<< "$out"; then
            echo "    note      : output contains a LeakSanitizer report"
        fi
        printf '%s\n' "$out" | sed 's/^/    > /'
        local out_nolsan
        out_nolsan=$(ASAN_OPTIONS="detect_leaks=0" "$TUR" -Xeffect-types check "$input" 2>&1)
        if printf '%s' "$out_nolsan" | grep -F -q "$needle"; then
            echo "    verdict   : '$needle' DOES appear with detect_leaks=0 --"
            echo "                cause is the leak-check exit truncating output,"
            echo "                NOT a type-checker divergence."
        else
            echo "    verdict   : '$needle' still absent with detect_leaks=0 --"
            echo "                genuine diagnostic divergence on this platform."
            echo "    --- detect_leaks=0 output (${#out_nolsan} bytes) ---"
            printf '%s\n' "$out_nolsan" | sed 's/^/    > /'
        fi
        return
    fi
    # The real assertion: no LeakSanitizer report.
    if grep -q "LeakSanitizer: detected memory leaks" <<< "$out"; then
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
