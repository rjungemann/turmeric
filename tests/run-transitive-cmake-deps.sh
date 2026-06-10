#!/usr/bin/env bash
# tests/run-transitive-cmake-deps.sh -- regression checks for the
# transitive `:cmake-deps` resolver (docs/archive/history/transitive-cmake-deps-plan.md).
#
# Covers the two cheap-and-deterministic cases the plan asks for:
#   1. transitive-cmake-deps-cycle    -- A→B→A in :spices; resolver must
#                                        terminate via its visited set.
#   2. transitive-cmake-deps-conflict -- two siblings declare the same
#                                        :cmake-deps name with different :refs;
#                                        resolver must emit the "conflicting
#                                        :cmake-deps" diagnostic and exit
#                                        non-zero before invoking cmake.
#
# The plan's third case (transitive-cmake-deps-basic, an end-to-end build
# through a sibling's :cmake-deps) is exercised by the cascade fixture in
# `../turmeric-spices/spices/tourist/tests/fixtures/cascade/` -- it is not
# replicated here because a hermetic version would need a vendored cmake
# project and a working `cmake` on the runner.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR_REL="./build/tur"
TUR_ABS="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
WORK="$(mktemp -d -t tur-tcd.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR_ABS" ]; then
    echo "tests: $TUR_ABS not built; run 'just build' first" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. Cycle terminates and the program builds + runs.
#    Uses an off-tree copy so the fixture's `build/` is hermetic.
CYCLE_SRC="tests/fixtures/transitive-cmake-deps-cycle"
CYCLE_DIR="$WORK/cycle"
cp -r "$CYCLE_SRC" "$CYCLE_DIR"
if timeout 30 "$TUR_ABS" build "$CYCLE_DIR/spice-a" \
        >"$WORK/cycle.log" 2>&1; then
    if [ ! -x "$CYCLE_DIR/spice-a/build/bin/spice-a" ]; then
        fail "transitive-cmake-deps-cycle-builds" \
             "binary missing at $CYCLE_DIR/spice-a/build/bin/spice-a"
    else
        out="$("$CYCLE_DIR/spice-a/build/bin/spice-a" 2>&1)"
        if [ "$out" = "cycle-ok" ]; then
            pass "transitive-cmake-deps-cycle-builds"
        else
            fail "transitive-cmake-deps-cycle-builds" \
                 "unexpected program output: '$out'"
        fi
    fi
else
    fail "transitive-cmake-deps-cycle-builds" \
         "tur build exit=$?: $(cat "$WORK/cycle.log")"
fi

# ---------------------------------------------------------------------------
# 2. Mismatched same-name :cmake-deps across siblings must fail with the
#    conflict diagnostic; cmake is never invoked.
CONF_SRC="tests/fixtures/transitive-cmake-deps-conflict"
CONF_DIR="$WORK/conflict"
cp -r "$CONF_SRC" "$CONF_DIR"
(
    cd "$CONF_DIR" || exit 3
    "$TUR_ABS" fetch >fetch.log 2>&1
)
conf_rc=$?
if [ "$conf_rc" -eq 0 ]; then
    fail "transitive-cmake-deps-conflict-detected" \
         "expected non-zero exit; got rc=$conf_rc, log=$(cat "$CONF_DIR/fetch.log")"
elif ! grep -q 'conflicting :cmake-deps for "fakelib"' "$CONF_DIR/fetch.log"; then
    fail "transitive-cmake-deps-conflict-detected" \
         "missing conflict diagnostic; got: $(cat "$CONF_DIR/fetch.log")"
else
    pass "transitive-cmake-deps-conflict-detected"
fi

echo
echo "transitive-cmake-deps: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
