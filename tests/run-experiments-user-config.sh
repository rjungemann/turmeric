#!/usr/bin/env bash
# tests/run-experiments-user-config.sh -- UC-3 integration test for the
# user-level experiments file (docs/upcoming/user-config-experiments-plan.md).
#
# Drives the built `tur` against a synthetic $XDG_CONFIG_HOME so no real user
# file is touched, and asserts the precedence/suppression matrix:
#
#   1. no manifest + user file present          -> gated source compiles
#   2. no manifest + no user file               -> gated source fails
#   3. manifest with empty :experiments []      -> user file suppressed, fails
#   4. manifest with :experiments [other]       -> user file suppressed, fails
#   5. CLI --enable= beats an empty environment  -> compiles
#   6. `tur experiments` source column reads user-config
#   7. unknown experiment name in the file       -> TUR-E0310 + path, exit 2
#   8. unknown key in the file                    -> TUR-W0062 warning, compiles
#
# The gated feature is `forall-kinds`/`hkt-hrt`: the probe source uses a
# kind-annotated forall bound variable, which requires --enable=forall-kinds
# (and hkt-hrt for the rank-2 instantiation). When those experiments graduate,
# swap the probe for a still-gated one.

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

# The probe source: a kind-annotated rank-2 forall use that needs
# --enable=forall-kinds,hkt-hrt. Copied from an existing compiled fixture so
# the two stay in sync on the syntax the gate keys on.
PROBE_SRC="tests/fixtures/hrt-hkt-option-instantiation/input.tur"
[ -f "$PROBE_SRC" ] || { echo "tests: probe source $PROBE_SRC missing" >&2; exit 2; }

# --- XDG homes -------------------------------------------------------------
# xdg-yes: enables the gated experiments.
XDG_YES="$WORK/xdg-yes"; mkdir -p "$XDG_YES/turmeric"
printf ';; test config\n:enable [forall-kinds\n         hkt-hrt]\n' \
    > "$XDG_YES/turmeric/experiments.tur"
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

# --- Scenario 1: no manifest + user file present -> compiles ---------------
S1="$WORK/s1"; mkdir -p "$S1"; cp "$PROBE_SRC" "$S1/input.tur"
assert_exit 0 "1. no-manifest + user file enables gated source" \
    "$XDG_YES" "$S1" emit-c input.tur

# --- Scenario 2: no manifest + no user file -> fails -----------------------
assert_exit 1 "2. no-manifest + no user file leaves gate closed" \
    "$XDG_EMPTY" "$S1" emit-c input.tur

# --- Scenario 3: manifest with empty :experiments [] -> suppressed ---------
S3="$WORK/s3"; mkdir -p "$S3/src"
printf '(defpackage "demo" :version "0.1.0" :experiments [])\n' > "$S3/build.tur"
cp "$PROBE_SRC" "$S3/src/input.tur"
assert_exit 1 "3. empty :experiments [] suppresses the user file" \
    "$XDG_YES" "$S3" emit-c src/input.tur

# --- Scenario 4: manifest with :experiments [other] -> suppressed ----------
S4="$WORK/s4"; mkdir -p "$S4/src"
printf '(defpackage "demo" :version "0.1.0" :experiments [vl-wide-functor])\n' \
    > "$S4/build.tur"
cp "$PROBE_SRC" "$S4/src/input.tur"
assert_exit 1 "4. non-overlapping :experiments suppresses the user file" \
    "$XDG_YES" "$S4" emit-c src/input.tur

# --- Scenario 5: CLI --enable= beats an empty environment ------------------
assert_exit 0 "5. CLI --enable= compiles with no user file" \
    "$XDG_EMPTY" "$S1" emit-c --enable=forall-kinds,hkt-hrt input.tur

# --- Scenario 6: `tur experiments` source column reads user-config ---------
exp_out=$(mktemp)
( cd "$S1" && XDG_CONFIG_HOME="$XDG_YES" "$TUR" experiments >"$exp_out" 2>/dev/null )
if grep -q "forall-kinds" "$exp_out" && grep "forall-kinds" "$exp_out" | grep -q "user-config"; then
    pass "6. tur experiments shows source user-config"
else
    failed "6. tur experiments source column"
    echo "  output:"; sed 's/^/    /' "$exp_out"
fi
rm -f "$exp_out"

# --- Scenario 7: unknown experiment name -> TUR-E0310 + path, exit 2 -------
XDG_BAD="$WORK/xdg-bad"; mkdir -p "$XDG_BAD/turmeric"
printf ':enable [not-a-real-experiment]\n' > "$XDG_BAD/turmeric/experiments.tur"
bad_err=$(mktemp)
( cd "$S1" && XDG_CONFIG_HOME="$XDG_BAD" "$TUR" emit-c input.tur >/dev/null 2>"$bad_err" )
bad_rc=$?
if [ "$bad_rc" -eq 2 ] \
    && grep -q "TUR-E0310" "$bad_err" \
    && grep -q "not-a-real-experiment" "$bad_err" \
    && grep -q "$XDG_BAD/turmeric/experiments.tur" "$bad_err"; then
    pass "7. unknown name -> TUR-E0310 with path, exit 2"
else
    failed "7. unknown name diagnostic (exit=$bad_rc)"
    echo "  stderr:"; sed 's/^/    /' "$bad_err"
fi
rm -f "$bad_err"

# --- Scenario 8: unknown key -> TUR-W0062 warning, still compiles ----------
XDG_KEY="$WORK/xdg-key"; mkdir -p "$XDG_KEY/turmeric"
printf ':bogus-key [a b]\n:enable [forall-kinds hkt-hrt]\n' \
    > "$XDG_KEY/turmeric/experiments.tur"
key_err=$(mktemp)
( cd "$S1" && XDG_CONFIG_HOME="$XDG_KEY" "$TUR" emit-c input.tur >/dev/null 2>"$key_err" )
key_rc=$?
if [ "$key_rc" -eq 0 ] && grep -q "TUR-W0062" "$key_err" && grep -q "bogus-key" "$key_err"; then
    pass "8. unknown key -> TUR-W0062 warning, still compiles"
else
    failed "8. unknown key handling (exit=$key_rc)"
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
