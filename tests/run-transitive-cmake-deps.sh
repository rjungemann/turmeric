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
# Plus two cases for the *include-path* walk, which is a separate traversal
# over the same :spices graph and needed its own visited set
# (docs/archive/history/spice-cycle-include-path-blowup.md):
#   4. spice-cycle-three-hop          -- a -> b -> c -> a; terminates, and
#                                        every src/ on the ring still lands on
#                                        the include path.
#   5. spice-diamond-shared-dep       -- one dep reached by two parents; the
#                                        visited set must dedupe the WALK
#                                        without pruning the include dir.
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

# ---------------------------------------------------------------------------
# 3. Workspace member seeding (workspace-transitive-native-deps-plan):
#    member-b does NOT list member-a in :spices, reaching it only through
#    the workspace :members resolution.  The transitive resolver must seed
#    member-a's :cmake-deps from :members; because both members declare the
#    same-named "fakelib" with different :refs, building member-b must emit
#    the conflict diagnostic.  Before the fix member-b saw only its own dep
#    and `tur fetch` succeeded (no conflict) -- this asserts the new seeding.
WMD_SRC="tests/fixtures/workspace-member-cmake-deps"
WMD_DIR="$WORK/wmd"
cp -r "$WMD_SRC" "$WMD_DIR"
(
    cd "$WMD_DIR/member-b" || exit 3
    "$TUR_ABS" fetch >fetch.log 2>&1
)
wmd_rc=$?
if [ "$wmd_rc" -eq 0 ]; then
    fail "workspace-member-cmake-deps-seed" \
         "expected non-zero exit (sibling dep should conflict); got rc=$wmd_rc, log=$(cat "$WMD_DIR/member-b/fetch.log")"
elif ! grep -q 'conflicting :cmake-deps for "fakelib"' "$WMD_DIR/member-b/fetch.log"; then
    fail "workspace-member-cmake-deps-seed" \
         "missing conflict diagnostic (sibling :cmake-deps not seeded from :members); got: $(cat "$WMD_DIR/member-b/fetch.log")"
else
    pass "workspace-member-cmake-deps-seed"
fi

# ---------------------------------------------------------------------------
# 4/5. spice-cycle-include-path-blowup: the transitive :spices walk that unions
#      dep src/ dirs into the include path needs the same visited set the
#      :cmake-deps walk above already has. Without one, a manifest cycle
#      recursed forever -- each lap resolved through one more `../` hop, so the
#      paths never repeated textually and the output dedup never fired. It
#      surfaced as a multi-kilobyte `-I` that cc rejected with "File name too
#      long" on an unrelated system header, or as a stack overflow under ASan.
#
#      Case 1 above covers the two-spice ring. These two cover the ways a
#      too-clever fix goes wrong: a three-hop ring (which a "is this dep my
#      parent?" check would miss) and a diamond (which an over-eager visited
#      set would prune, dropping an include dir that is genuinely needed).
run_spice_walk_case() {  # <fixture> <member> <binary> <expected-output>
    local fixture="$1" member="$2" bin="$3" want="$4"
    local name="${fixture}-builds"
    local src="tests/fixtures/$fixture"
    local dir="$WORK/$fixture"
    cp -r "$src" "$dir"
    if timeout 30 "$TUR_ABS" build "$dir/$member" >"$WORK/$fixture.log" 2>&1; then
        if [ ! -x "$dir/$member/build/bin/$bin" ]; then
            fail "$name" "binary missing at $dir/$member/build/bin/$bin"
        else
            local out
            out="$("$dir/$member/build/bin/$bin" 2>&1)"
            if [ "$out" = "$want" ]; then
                pass "$name"
            else
                fail "$name" "expected '$want', got '$out'"
            fi
        fi
    else
        fail "$name" "tur build exit=$?: $(cat "$WORK/$fixture.log")"
    fi
}

run_spice_walk_case spice-cycle-three-hop     a   a   "three-hop-ok"
run_spice_walk_case spice-diamond-shared-dep  app app "diamond-ok"

echo
echo "transitive-cmake-deps: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
