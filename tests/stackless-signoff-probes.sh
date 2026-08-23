#!/usr/bin/env bash
#
# Sign-off probes for the stackless / heap-continuation control lowerings.
#
# Runs each probe shape at 1,000,000 depth under a REDUCED stack
# (ulimit -s 256) and asserts each exits 0 with the expected value.
#
#   - cu-rec / cu-catch-deep / atom-rec / mutual-rec / fiber-rec are the five
#     (graduated) stackless catch-unwind shapes from
#     docs/archive/history/catch-unwind-graduation-plan.md Part A.  At this depth the
#     native (non-trampolined) lowering exhausts the C stack for the
#     catch-nesting shapes; the flat-stack trampoline is what lets them finish.
#   - effect-rec is the Phase F4 probe from
#     docs/archive/compiled-first-class-continuations-plan.md: a nested
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

# Per-probe wall-clock cap.  A probe that runs longer than this is treated as a
# hang and killed, so a NON-TERMINATING probe surfaces as a loud failure instead
# of stalling the whole CI job indefinitely.  (This guard exists because a probe
# added without it hung the Ubuntu/macOS `ctest` job until GitHub canceled it ~32
# minutes in.)  `perl -e 'alarm N; exec @ARGV'` is the portable timeout used
# across this repo -- coreutils `timeout` is absent on the macOS runners; SIGALRM
# survives `exec` and terminates the child, so a hang exits 142 rather than
# waiting forever.  A healthy probe finishes in well under a second.
TIMEOUT_S="${TUR_PROBE_TIMEOUT_S:-60}"
# Known-broken probes are capped tighter: they either crash instantly or hang, so
# there is no point burning a full TIMEOUT_S per CI run re-confirming they are
# still broken.
XFAIL_TIMEOUT_S="${TUR_PROBE_XFAIL_TIMEOUT_S:-20}"

if [ ! -x "$TUR" ]; then
    echo "stackless-signoff: compiler not found at $TUR (set TUR=...)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# probe name -> expected stdout value
names=(cu-rec cu-catch-deep atom-rec mutual-rec fiber-rec effect-rec async-rec)
declare -A expect=(
    [cu-rec]=1000000
    [cu-catch-deep]=1000000
    [atom-rec]=1000000
    [mutual-rec]=1000001
    [fiber-rec]=1000000
    [effect-rec]=1000000
    [async-rec]=1000000
)

# Per-probe extra build flags (on top of $ENABLE).  Empty since 2026-08-22.
#
# async-rec used to be built with --enable=cps-async, "so the flag is exercised
# end-to-end".  That stopped being true in two steps: cps-async GRADUATED
# 2026-07-19, making the flag a TUR-W0063 no-op, and its GRADUATED[] shim was
# then retired at 0.37.0, making it the hard TUR-E0310 an unknown experiment
# name gets -- which failed this probe with `build error` rather than anything
# about stacklessness.
#
# Dropping the flag loses nothing.  What the probe measures is the async shape
# at 1,000,000 depth under a 256KB stack, and the lowering it measures is
# unconditional now; recursive await still evicts to the direct emitter (F3.5),
# exactly as it did with the flag on.  The table itself stays for the next probe
# that needs a real per-probe flag -- `${pflags[$p]:-}` is empty-safe.
declare -A pflags=()

# Probes that expose a genuine, still-open runtime bug are listed here.  They
# stay in the rotation as live regression guards but are EXPECTED to fail, so
# they do not fail the suite -- the alternative (a hard failure, or worse a
# hang) is what stalled CI.  If one starts PASSING it is reported as an
# unexpected XPASS and DOES fail the suite, prompting its removal from this list
# and re-arming it as a normal probe.
#
# The list is currently empty -- all probes pass:
#   - effect-rec graduated 2026-07-24 (nested-handler non-termination fixed;
#     docs/archive/effect-rec-nested-handler-nonterminates.md).
#   - fiber-rec  graduated 2026-07-24 (async + deep catch-unwind no longer
#     evicted from the flat-stack trampoline; a CPS-marked clone kept the
#     stackless lowering -- docs/archive/fiber-rec-async-fiber-segfault.md).
declare -A xfail=()

# Run $bin under a reduced stack ($STACK_KB) with a hard wall-clock cap ($2
# seconds).  Echoes combined stdout+stderr and returns the child's exit status
# (142 when SIGALRM fires, i.e. the probe hung and was killed).
run_probe() {
    local bin="$1" cap="$2"
    ( ulimit -s "$STACK_KB"; exec perl -e 'alarm shift; exec @ARGV' "$cap" "$bin" ) 2>&1
}

fails=0
xfails=0
for p in "${names[@]}"; do
    src="$PROBE_DIR/$p.tur"
    bin="$WORK/$p.bin"
    is_xfail="${xfail[$p]:-}"
    if ! "$TUR" $ENABLE ${pflags[$p]:-} build "$src" -o "$bin" >"$WORK/$p.build.log" 2>&1; then
        if [ -n "$is_xfail" ]; then
            echo "XFAIL $p: build error (known-broken)"; xfails=$((xfails+1))
        else
            echo "FAIL $p: build error"; sed 's/^/    /' "$WORK/$p.build.log"; fails=$((fails+1))
        fi
        continue
    fi
    cap="$TIMEOUT_S"; [ -n "$is_xfail" ] && cap="$XFAIL_TIMEOUT_S"
    out="$(run_probe "$bin" "$cap")"; rc=$?
    exp="${expect[$p]}"
    passed=0
    if [ "$rc" = "0" ] && [ "$out" = "$exp" ]; then passed=1; fi
    note=""
    if [ "$rc" = "142" ]; then note=" (timed out after ${cap}s -- treated as hang)"; fi
    if [ -n "$is_xfail" ]; then
        if [ "$passed" = "1" ]; then
            echo "XPASS $p: $out -- now passing; remove it from the xfail list to re-arm the guard"
            fails=$((fails+1))
        else
            echo "XFAIL $p: rc=$rc out='$out' expected='$exp'$note (known-broken)"
            xfails=$((xfails+1))
        fi
    else
        if [ "$passed" = "1" ]; then
            echo "PASS $p: $out (1000000-depth, ${STACK_KB}KB stack)"
        else
            echo "FAIL $p: rc=$rc out='$out' expected='$exp'$note"; fails=$((fails+1))
        fi
    fi
done

echo "---"
if [ "$fails" = "0" ]; then
    echo "stackless-signoff: ${#names[@]} probes checked, $xfails known-broken (xfail), all others passed"
    exit 0
else
    echo "stackless-signoff: $fails probe(s) failed, $xfails known-broken (xfail)"
    exit 1
fi
