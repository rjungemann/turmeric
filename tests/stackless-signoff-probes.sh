#!/usr/bin/env bash
#
# Sign-off probes for the stackless / heap-continuation control lowerings.
#
# Runs each probe shape at 1,000,000 depth under a REDUCED stack
# (ulimit -s 256) and asserts each exits 0 with the expected value.
#
#   - cu-rec / cu-catch-deep / atom-rec / mutual-rec / fiber-rec are the five
#     (graduated) stackless catch-unwind shapes from
#     docs/archive/catch-unwind-graduation-plan.md Part A.  At this depth the
#     native (non-trampolined) lowering exhausts the C stack for the
#     catch-nesting shapes; the flat-stack trampoline is what lets them finish.
#   - effect-rec is the Phase F4 probe from
#     docs/upcoming/v1/compiled-first-class-continuations-plan.md: a nested
#     effect handler around a deep perform/resume loop.  On the fiber-based
#     effect runtime it already completes in bounded C stack (each handle body
#     is its own fiber; resume re-enters it iteratively); it is the standing
#     regression guard for that property once effects move onto heap
#     continuations (Phase F1).
#
# These runs are slow / memory-heavy, so they are deliberately kept OUT of
# the default `tests/run.sh` fast suite.  Invoke this script directly, or via
# its dedicated ctest target:
#
#     bash tests/stackless-signoff-probes.sh
#     ctest --test-dir build -R stackless_signoff_probes --output-on-failure
#
# The lowering is always-on since graduation, so no --enable flag is needed.
# To exercise it against a build that still gates the feature behind the
# retired experiment, export TUR_STACKLESS_ENABLE=--enable=stackless-catch-unwind.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
PROBE_DIR="$HERE/probes/stackless-signoff"
ENABLE="${TUR_STACKLESS_ENABLE:-}"
STACK_KB="${TUR_PROBE_STACK_KB:-256}"

if [ ! -x "$TUR" ]; then
    echo "stackless-signoff: compiler not found at $TUR (set TUR=...)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# probe name -> expected stdout value
names=(cu-rec cu-catch-deep atom-rec mutual-rec fiber-rec effect-rec)
declare -A expect=(
    [cu-rec]=1000000
    [cu-catch-deep]=1000000
    [atom-rec]=1000000
    [mutual-rec]=1000001
    [fiber-rec]=1000000
    [effect-rec]=1000000
)

fails=0
for p in "${names[@]}"; do
    src="$PROBE_DIR/$p.tur"
    bin="$WORK/$p.bin"
    if ! "$TUR" $ENABLE build "$src" -o "$bin" >"$WORK/$p.build.log" 2>&1; then
        echo "FAIL $p: build error"; sed 's/^/    /' "$WORK/$p.build.log"; fails=$((fails+1)); continue
    fi
    out="$( (ulimit -s "$STACK_KB"; "$bin") 2>&1 )"; rc=$?
    exp="${expect[$p]}"
    if [ "$rc" = "0" ] && [ "$out" = "$exp" ]; then
        echo "PASS $p: $out (1000000-depth, ${STACK_KB}KB stack)"
    else
        echo "FAIL $p: rc=$rc out='$out' expected='$exp'"; fails=$((fails+1))
    fi
done

echo "---"
if [ "$fails" = "0" ]; then
    echo "stackless-signoff: all ${#names[@]} probes passed"
    exit 0
else
    echo "stackless-signoff: $fails probe(s) failed"
    exit 1
fi
