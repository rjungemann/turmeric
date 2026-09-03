#!/usr/bin/env bash
# tests/run-option-niche-seam.sh -- keep the option-niche experiment's green
# state from rotting while it soaks toward a graduation call.
#
# `--enable=option-niche` carries an eligible `(Option P)` as its payload
# pointer (docs/upcoming/sr3-option-niche-plan.md).  The experiment is OFF by
# default, so the ordinary suite only exercises the niche path through the
# three fixtures that carry a `flags` file; everything else -- the
# (Option String) census population, the inline-C Option builders, the
# eligible-collection shapes -- compiles the default representation.  A gate
# nobody turns on decays to a gate nobody notices (the SR4 seam harness's
# lesson), and five silent-wrong-answer crossings were found in this
# representation's first two days; this harness is what makes "no new crossing
# defects over the soak window" a checkable claim instead of a mood.
#
# Two checks, mirroring run-sr4-seam.sh:
#   1. A CANARY: emit-c of httpd-req-string-opt under the flag must show the
#      niche ctor (`static void * ctor_Option_Some__String(`) and must NOT mint the
#      tagged monomorph typedef.  If the flag is renamed or the gate stops
#      reading it, this fails LOUDLY instead of silently re-testing default.
#   2. The niche-eligible population, compiled and RUN under the flag against
#      each fixture's committed expected.stdout.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
ENABLE="--enable=option-niche"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL option-niche-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 30"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-option-niche-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# Fixture binaries link libturi like the main suite's do.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

# --- 1. Canary: the flag must actually bite. ------------------------------
canary_c="$WORKDIR/canary.c"
if ! $TIMEOUT "$TUR" $ENABLE emit-c tests/fixtures/httpd-req-string-opt/input.tur \
        > "$canary_c" 2>/dev/null; then
    fail "option-niche-canary" "emit-c of httpd-req-string-opt failed under $ENABLE"
elif ! grep -q "static void \* ctor_Option_Some__String(" "$canary_c"; then
    fail "option-niche-canary" \
        "ctor_Option_Some__String is not the niche identity -- the flag did not bite.  If \
the experiment was renamed or adt_app_is_niche_option moved, update this \
harness in the same change."
elif grep -q "typedef struct tur_adt_Option__String" "$canary_c"; then
    fail "option-niche-canary" \
        "the tagged tur_adt_Option__String typedef was minted despite the niche"
else
    pass "option-niche-canary (ctor_Option_Some__String is the identity, no typedef)"
fi

# --- 2. The eligible population, compiled and RUN under the flag. ---------
# option-niche-*: the experiment's own fixtures (their flags files make the
#   ordinary suite cover them; running them here too is deliberate overlap).
# httpd-req-string-opt / bound-string / inline-c-option-byval-param: the
#   (Option String) census shapes -- header lookups, bounds, inline-C
#   builders straddling the by-value param ABI.
# inline-c-result-builder / inline-c-typed-result-option: Result stays
#   carrier/by-value next to a niche Option in one program.
# option-of-tvec-eq: the original SR3 gate fixture -- an eligible
#   option<Vec int> through a typeclass Eq dictionary's erased crossing.
FIXTURES="
option-niche-string
option-niche-crossings
option-niche-vec-word
httpd-req-string-opt
bound-string
inline-c-option-byval-param
inline-c-result-builder
inline-c-typed-result-option
option-of-tvec-eq
"

for fx in $FIXTURES; do
    dir="tests/fixtures/$fx"
    input="$dir/input.tur"
    expected="$dir/expected.stdout"
    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        fail "$fx" "fixture missing (input.tur or expected.stdout)"
        continue
    fi
    exe="$WORKDIR/$fx.bin"
    mkdir -p "$(dirname "$exe")"
    if ! $TIMEOUT "$TUR" $ENABLE build "$input" -o "$exe" > /dev/null 2>"$WORKDIR/err"; then
        fail "$fx" "build failed under $ENABLE: $(tail -2 "$WORKDIR/err" | head -1)"
        continue
    fi
    if ! $TIMEOUT "$exe" > "$WORKDIR/out" 2>/dev/null; then
        fail "$fx" "run failed (nonzero exit) under $ENABLE"
        continue
    fi
    if ! diff -q "$expected" "$WORKDIR/out" > /dev/null 2>&1; then
        fail "$fx" "stdout mismatch under $ENABLE"
        continue
    fi
    pass "$fx"
done

echo
echo "option-niche-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
