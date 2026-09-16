#!/usr/bin/env bash
# tests/run-experiments-user-config.sh -- UC-3 integration test for the
# user-level experiments file (docs/archive/history/user-config-experiments-plan.md).
#
# Drives the built `tur` against a synthetic $XDG_CONFIG_HOME so no real user
# file is touched.
#
# The registry was EMPTY from 2026-07-06 (forall-dict-pass, the probe this
# test used to key on, graduated) until 2026-09-16, when `class-superclasses`
# registered.  The registry-INDEPENDENT paths -- the parts of the mechanism
# that must hold regardless of what is registered -- are covered first:
#
#   A. manifest with an unknown :experiments name  -> TUR-E0310, exit 2
#   B. user file with an unknown experiment name    -> TUR-E0310 + path, exit 2
#   C. user file with an unknown key                 -> TUR-W0062 warning, compiles
#   D. absent user file                              -> no-op, compiles
#
# Then the gated-source probe, keyed on the FIRST registered experiment and a
# source that only compiles with it enabled (the enable/precedence matrix):
#
#   E. gated source, nothing enabled                 -> refused (exit != 0)
#   F. gated source, user file :enable [<probe>]     -> compiles (TUR-W006x)
#   G. same user file + manifest `:experiments []`    -> user file suppressed,
#                                                        refused again
#
# E-G run only while a gated probe source exists for the first registered
# experiment (GATED_SRC below); if the registry empties again they skip, and
# the next experiment to register should point GATED_SRC at its own fixture.

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

# --- E-G. gated-source probe (only with a live experiment) ------------------
# `class-superclasses` gates the defclass constraint preamble; the fixture
# below is a hard error without the enable (TUR-E0390) and compiles with it.
PROBE_NAME="class-superclasses"
GATED_SRC="tests/fixtures/class-superclass-entails/input.tur"
if "$TUR" experiments 2>/dev/null | grep -q "^$PROBE_NAME " && [ -f "$GATED_SRC" ]; then
    S5="$WORK/s5"; mkdir -p "$S5"; cp "$GATED_SRC" "$S5/input.tur"
    assert_exit 1 "E. gated source refused with nothing enabled" \
        "$XDG_EMPTY" "$S5" emit-c input.tur

    XDG_ON="$WORK/xdg-on"; mkdir -p "$XDG_ON/turmeric"
    printf ':enable [%s]\n' "$PROBE_NAME" > "$XDG_ON/turmeric/experiments.tur"
    assert_exit 0 "F. gated source compiles via user-file :enable" \
        "$XDG_ON" "$S5" emit-c input.tur

    S6="$WORK/s6"; mkdir -p "$S6/src"
    printf '(defpackage "demo" :version "0.1.0" :experiments [])\n' > "$S6/build.tur"
    cp "$GATED_SRC" "$S6/src/input.tur"
    assert_exit 1 "G. manifest :experiments [] suppresses the user file" \
        "$XDG_ON" "$S6" emit-c src/input.tur
else
    echo "skip  E-G. no gated probe source for the first registered experiment"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
