#!/usr/bin/env bash
# tests/run-flags.sh — Tests for global flags that can't be expressed as
# standard fixture tests (--explain, --dump-kinds, etc.)
#
# Each test case is a short bash block that prints "PASS <name>" on success
# or "FAIL <name> — <reason>" on failure, then increments the counter.
#
# Exit status: 0 if all tests pass, 1 if any fail.
#
# Phase HKT-P5: tur-explain-kind-mismatch
# Phase HKT-P6: dump-kinds-basic

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="./build/tur"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 — $2"; }

# ---------------------------------------------------------------------------
# HKT-P5 tests
# ---------------------------------------------------------------------------

# tur-explain-kind-mismatch: --explain TUR-E0012 should produce non-empty
# output containing "Kind" and exit 0.
out=$("$TUR" --explain TUR-E0012 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-kind-mismatch" "non-zero exit ($rc)"
elif [ -z "$out" ]; then
    fail "tur-explain-kind-mismatch" "empty output"
elif ! echo "$out" | grep -qi "Kind"; then
    fail "tur-explain-kind-mismatch" "output did not mention 'Kind'"
else
    pass "tur-explain-kind-mismatch"
fi

# tur-explain-orphan-instance: --explain TUR-E0013 should mention "orphan"
out=$("$TUR" --explain TUR-E0013 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-orphan-instance" "non-zero exit ($rc)"
elif ! echo "$out" | grep -qi "orphan\|instance"; then
    fail "tur-explain-orphan-instance" "output did not mention 'orphan/instance'"
else
    pass "tur-explain-orphan-instance"
fi

# tur-explain-all-codes: every known TUR-E code should have an explanation
for code in TUR-E0001 TUR-E0002 TUR-E0003 TUR-E0004 TUR-E0005 \
            TUR-E0007 TUR-E0009 TUR-E0010 TUR-E0011 TUR-E0012 TUR-E0013; do
    out=$("$TUR" --explain "$code" 2>&1); rc=$?
    if [ $rc -ne 0 ] || [ -z "$out" ]; then
        fail "tur-explain-${code}" "no explanation registered (exit=$rc)"
    else
        pass "tur-explain-${code}"
    fi
done

# tur-explain-unknown-code: --explain TUR-E9999 should exit non-zero
out=$("$TUR" --explain TUR-E9999 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "tur-explain-unknown-code" "expected non-zero exit, got 0"
else
    pass "tur-explain-unknown-code"
fi

# ---------------------------------------------------------------------------
# HKT-P6 tests
# ---------------------------------------------------------------------------

# dump-kinds-basic: --dump-kinds emit-c on a file with a KIND_ARROW typeclass
# should print "defclass Functor param[0] : * -> *" to stdout.
FIXTURE="tests/fixtures/dump-kinds-basic/input.tur"
out=$("$TUR" --dump-kinds emit-c "$FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-kinds-basic" "non-zero exit ($rc)"
elif ! echo "$out" | grep -q "defclass Functor param\[0\] : \* -> \*"; then
    fail "dump-kinds-basic" "expected 'defclass Functor param[0] : * -> *' in output"
else
    pass "dump-kinds-basic"
fi

# dump-kinds-no-output: without --dump-kinds, the same file should NOT print
# kind annotations mixed into the C output.
out=$("$TUR" emit-c "$FIXTURE" 2>/dev/null); rc=$?
if echo "$out" | grep -q "defclass Functor param"; then
    fail "dump-kinds-no-output" "kind dump appeared without --dump-kinds flag"
else
    pass "dump-kinds-no-output"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "flags summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
