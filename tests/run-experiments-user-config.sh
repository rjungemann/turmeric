#!/usr/bin/env bash
# tests/run-experiments-user-config.sh -- UC-3 integration test for the
# user-level experiments file (docs/archive/history/user-config-experiments-plan.md).
#
# Drives the built `tur` against a synthetic $XDG_CONFIG_HOME so no real user
# file is touched.
#
# As of 2026-07-06 the experiment registry is EMPTY: forall-dict-pass -- the
# last surviving flag, and the probe this test used to key on -- graduated to
# always-on (docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md).  With
# no experiment registered, the "enabling a flag changes compile behavior"
# scenarios (a gated source that only compiles with --enable=<x>) are not
# expressible -- there is no gated feature to probe.  This test therefore
# covers the registry-INDEPENDENT paths, which are the parts of the mechanism
# that must hold regardless of what is registered:
#
#   A. manifest with an unknown :experiments name  -> TUR-E0310, exit 2
#   B. user file with an unknown experiment name    -> TUR-E0310 + path, exit 2
#   C. user file with an unknown key                 -> TUR-W0062 warning, compiles
#   D. absent user file                              -> no-op, compiles
#
# When a new experiment is registered, restore a gated-source probe here (and
# in tests/unit/experiments_user_config.c) to re-cover the enable/precedence
# matrix -- see git history for the forall-dict-pass form.

set -u
cd "$(dirname "$0")/.."

TUR="$PWD/build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

# Isolate from a globally-exported stdlib dir, matching tests/run.sh.
unset TUR_STDLIB_DIR

WORK=$(mktemp -d -t tur-uc-int.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
FAILED=()

pass() { echo "PASS $1"; PASS=$((PASS + 1)); }
failed() { echo "FAIL $1"; FAIL=$((FAIL + 1)); FAILED+=("$1"); }

# A source that compiles cleanly with no flags -- used to observe that the
# user-config read is a transparent no-op / warning (not a hard error) on the
# paths that should still compile.
SRC="tests/fixtures/forall-dict-show/input.tur"
[ -f "$SRC" ] || { echo "tests: source $SRC missing" >&2; exit 2; }

# xdg-empty: exists but carries no turmeric/experiments.tur.
XDG_EMPTY="$WORK/xdg-empty"; mkdir -p "$XDG_EMPTY"

# assert_exit WANT LABEL XDG_DIR RUN_DIR -- ARGS...
assert_exit() {
    local want="$1" label="$2" xdg="$3" rundir="$4"; shift 4
    local err rc
    err=$(mktemp)
    ( cd "$rundir" && XDG_CONFIG_HOME="$xdg" "$TUR" "$@" >/dev/null 2>"$err" )
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        pass "$label"
    else
        failed "$label -- expected exit $want, got $rc"
        echo "  cmd: (cd $rundir; XDG_CONFIG_HOME=$xdg tur $*)"
        echo "  stderr:"; sed 's/^/    /' "$err"
    fi
    rm -f "$err"
}

S1="$WORK/s1"; mkdir -p "$S1"; cp "$SRC" "$S1/input.tur"

# --- D. absent user file -> no-op, compiles -------------------------------
assert_exit 0 "D. no user file compiles" \
    "$XDG_EMPTY" "$S1" emit-c input.tur

# --- A. manifest with an unknown :experiments name -> TUR-E0310, exit 2 ----
S4="$WORK/s4"; mkdir -p "$S4/src"
printf '(defpackage "demo" :version "0.1.0" :experiments [not-a-real-experiment])\n' \
    > "$S4/build.tur"
cp "$SRC" "$S4/src/input.tur"
assert_exit 2 "A. unknown :experiments name aborts with TUR-E0310" \
    "$XDG_EMPTY" "$S4" emit-c src/input.tur

# --- B. user file with an unknown experiment name -> TUR-E0310 + path ------
XDG_BAD="$WORK/xdg-bad"; mkdir -p "$XDG_BAD/turmeric"
printf ':enable [not-a-real-experiment]\n' > "$XDG_BAD/turmeric/experiments.tur"
bad_err=$(mktemp)
( cd "$S1" && XDG_CONFIG_HOME="$XDG_BAD" "$TUR" emit-c input.tur >/dev/null 2>"$bad_err" )
bad_rc=$?
if [ "$bad_rc" -eq 2 ] \
    && grep -q "TUR-E0310" "$bad_err" \
    && grep -q "not-a-real-experiment" "$bad_err" \
    && grep -q "$XDG_BAD/turmeric/experiments.tur" "$bad_err"; then
    pass "B. unknown name -> TUR-E0310 with path, exit 2"
else
    failed "B. unknown name diagnostic (exit=$bad_rc)"
    echo "  stderr:"; sed 's/^/    /' "$bad_err"
fi
rm -f "$bad_err"

# --- C. unknown key -> TUR-W0062 warning, still compiles -------------------
# An empty :enable [] keeps this independent of what (if anything) is registered.
XDG_KEY="$WORK/xdg-key"; mkdir -p "$XDG_KEY/turmeric"
printf ':bogus-key [a b]\n:enable []\n' \
    > "$XDG_KEY/turmeric/experiments.tur"
key_err=$(mktemp)
( cd "$S1" && XDG_CONFIG_HOME="$XDG_KEY" "$TUR" emit-c input.tur >/dev/null 2>"$key_err" )
key_rc=$?
if [ "$key_rc" -eq 0 ] && grep -q "TUR-W0062" "$key_err" && grep -q "bogus-key" "$key_err"; then
    pass "C. unknown key -> TUR-W0062 warning, still compiles"
else
    failed "C. unknown key handling (exit=$key_rc)"
    echo "  stderr:"; sed 's/^/    /' "$key_err"
fi
rm -f "$key_err"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
