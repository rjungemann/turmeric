#!/usr/bin/env bash
# tests/run-sr2-seam.sh -- keep the SR2 seam's worked-down state from rotting.
#
# TUR_SR2_APP_SUM_BYVALUE=1 admits a MULTI-VARIANT PARAMETRIC sum monomorph
# ((Opt2 int), (PRes cstr), (ExprF B)) to the by-value path -- the real
# prerequisite for Option and Result as sums.  The seam is OFF by default, so
# nothing in the ordinary suite ever compiles a parametric sum by value: the
# fixtures below are green under the seam and NOTHING enforces that.  A gate
# nobody turns on decays to a gate nobody notices (the lesson of
# docs/reported/sanitizer-gate-not-armed-in-ci.md), and the SR2 gate document
# makes a specific claim -- its ELEVEN-fixture M7 worklist all green as of
# 2026-08-27 -- that this harness is what keeps true.
#
# As of that date the FULL suite is also green under the seam (2710/0), so this
# harness is a fast subset chosen for signal, not the whole claim.  When the
# seam moves, `TUR_SR2_APP_SUM_BYVALUE=1 bash tests/run.sh` is the real check;
# this one is what CI can afford to run every time.
#
# See docs/upcoming/sr2-gate-results.md.
#
# Two checks:
#   1. A CANARY: emit-c of a parametric-sum fixture must show the by-value ctor
#      signature.  Under the carrier the same ctor returns int64_t and mallocs,
#      so if the env var is ever renamed or the gate stops reading it, this
#      harness fails LOUDLY instead of silently re-testing the default path.
#   2. Every fixture below builds and prints its committed expected.stdout with
#      the seam ON.  VALUES, not just the build: this whole family's failures
#      hide behind function-pointer casts that cc cannot see through, and the
#      acceptance test (parsec-tutorial) used to build cleanly and SEGV.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export TUR_SR2_APP_SUM_BYVALUE=1
# The compiler binary is ASan-instrumented in Debug; emitted programs are not.
# Leak-checking the compile path stays the ordinary suite's job -- this harness
# answers "does the seam still produce correct programs".
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL sr2-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

# Stock macOS ships no timeout(1); mirror the other harnesses' fallback.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 60"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-sr2-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# --- 1. Canary: the seam must actually bite. ------------------------------
canary_c="$WORKDIR/canary.c"
if ! $TIMEOUT "$TUR" emit-c tests/fixtures/parsec-tutorial/input.tur \
        > "$canary_c" 2>/dev/null; then
    fail "sr2-seam-canary" "emit-c of parsec-tutorial failed under the seam"
elif grep -qE "^static tur_adt_PRes__[A-Za-z0-9_]+ ctor_POK__" "$canary_c"; then
    pass "sr2-seam-canary (ctor_POK__* returns its monomorph by value)"
else
    fail "sr2-seam-canary" \
        "ctor_POK__* is not by-value -- the seam did not bite.  If \
TUR_SR2_APP_SUM_BYVALUE was renamed or the sr2_app_sum_byvalue gate moved, \
update this harness in the same change."
fi

# --- 2. The M7 worklist fixtures that are green, compiled and RUN. ---------
# hkt-cata-*: the fixpoint-functor carrier family (ExprF/Expr, ReF/Re).
# hkt-fmap-byvalue-sum-element: a Functor instance over a parametric sum,
#   monomorphised per result element -- the discriminating probe reads the
#   SECOND child, so a layout mismatch shows up as a wrong value.
# class-method-hkt-tyvar-grounding: a method-level tyvar in result position,
#   and the nested-ctor field channel -- the inner `(None)` of `(Some (None))`.
# conv-with-narrowed-variant-parametric: the let use-site look-ahead -- an
#   unannotated `(Empty)` grounded from a later `(getv e)`.
# poly-combinator-application-element-inference: a closure lifted out of a
#   POLYMORPHIC combinator, cloned per instantiation so its dispatch is typed.
#   Pins a non-zero expected.exit.
# parsec-tutorial: the gate document's acceptance test.  PRes is the Option
#   shape verbatim, driven through parser-combinator closures.
FIXTURES="
hkt-cata-captureless-fn-carrier-arm
hkt-cata-fmap-byvalue-carrier
hkt-cata-fn-arg-carrier
hkt-cata-fn-carrier-recursive
hkt-cata-mixed-fn-value-carrier
hkt-cata-wide-byvalue-carrier
hkt-fmap-byvalue-sum-element
class-method-hkt-tyvar-grounding
conv-with-narrowed-variant-parametric
poly-combinator-application-element-inference
parsec-tutorial
"

for fx in $FIXTURES; do
    dir="tests/fixtures/$fx"
    input="$dir/input.tur"
    expected="$dir/expected.stdout"
    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        fail "$fx" "fixture missing (input.tur or expected.stdout)"
        continue
    fi
    bin="$WORKDIR/$(echo "$fx" | tr '/' '_').bin"
    build_out=$($TIMEOUT "$TUR" build "$input" -o "$bin" 2>&1)
    if [ $? -ne 0 ] || [ ! -x "$bin" ]; then
        fail "$fx" "build failed: $(echo "$build_out" | grep -m1 'error' || echo "$build_out" | tail -1)"
        continue
    fi
    actual=$($TIMEOUT "$bin" 2>/dev/null)
    rc=$?
    # A fixture may pin a non-zero exit (expected.exit), the way the main suite
    # does -- poly-combinator-application-element-inference returns 42 from
    # main by design.  Absent the file, 0 is the expectation.
    want_rc=0
    [ -f "$dir/expected.exit" ] && want_rc=$(cat "$dir/expected.exit")
    if [ "$rc" != "$want_rc" ]; then
        fail "$fx" "program exited $rc (expected $want_rc)"
        continue
    fi
    if [ "$actual" = "$(cat "$expected")" ]; then
        pass "$fx"
    else
        fail "$fx" "stdout mismatch (expected $(head -c 40 "$expected" | tr '\n' ' ')...)"
    fi
done

echo ""
echo "sr2-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
