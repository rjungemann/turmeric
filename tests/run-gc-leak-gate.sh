#!/usr/bin/env bash
# tests/run-gc-leak-gate.sh -- CG7 (gc-cycle-collection-followup-plan):
# run the COMPILED cycle-collector fixtures under AddressSanitizer, with a
# collector-off companion run as the control.
#
# tests/run.sh compiles fixture programs WITHOUT sanitizers -- only `tur` itself
# is instrumented -- so nothing in the normal suite ever runs a compiled binary
# under ASan. That is the gap this closes: a use-after-free or heap overflow in
# the collector's own sweep is invisible to the ordinary suite, which only
# checks the program's printed output. (CG5's mid-construction walk of a
# half-built block was exactly this shape of bug.)
#
# Each fixture is built with -fsanitize=address and run TWICE:
#
#   1. collector ON  -- must be ASan-clean, and must print its expected output.
#   2. collector OFF -- must be ASan-clean, and must print something DIFFERENT.
#
# The second run is the control. A passing run with the collector on proves
# little on its own; the cycle might be reclaimed by teardown, by the allocator,
# or never built. The paired run is what shows the collector is doing the work
# -- the same on/off control the CG2 measurement used.
#
# --------------------------------------------------------------------------
# Why this does NOT use LeakSanitizer, which is the obvious tool for the job:
#
# LSan reports memory unreachable from roots. Every rc block is held in the
# `gc_all_blocks` registry, a global -- so an uncollected cycle is *reachable*
# by construction and LSan calls it live. Measured: the collector-off run of
# gc-collects-strong-cycle retains ~1 MB of cycles and LSan reports zero leaks.
#
# The control is therefore built on the CG6 counters -- (gc-objects-freed),
# (gc-live-blocks) -- which report the collector's actual work regardless of
# which allocator is underneath. LSan would silently pass both sides of the
# pair, and so would a mallinfo2 probe under ASan (see the fixture lists).
# --------------------------------------------------------------------------
#
# Opt-in: a sanitized compile per fixture is slow, and it is a diagnostic gate
# rather than a correctness gate for everyday work.
#
# Exit status: 0 if every assertion holds, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
WORK="$(mktemp -d -t tur-gcleak.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
SKIP=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }
# Skips are counted and printed: a check this gate declines to make must not
# read as a check that passed.
skip() { SKIP=$((SKIP + 1)); echo "SKIP $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built" >&2
    exit 2
fi

# Fixtures that build unreachable rc<T> cycles and rely on the collector.
#
# Every fixture here gets the ASan-clean checks.  Nothing that reads its result
# from a malloc probe gets an OUTPUT check of any kind, because a malloc probe
# does not measure the program's own heap once ASan is underneath it.  Measured
# on the fixture that used to be the only entry here:
#
#            collector on   collector off
#   plain          0           1087232      <- the probe measures; run.sh asserts this
#   ASan           0                 0      <- the probe is blind
#
# Both consequences follow from that one row:
#
#   - The on/off CONTROL is impossible: identical output either way.
#   - The expected-output check is VACUOUS on glibc -- it compares 0 against an
#     expected 0 that the collector had no part in producing -- and on Darwin it
#     is worse than vacuous.  There the probe is malloc_zone_statistics on the
#     default zone, which reads whatever zone ASan installs, and ASan's
#     quarantine keeps freed blocks accounted as in-use; the fixture then prints
#     ~800000 (a clean 160 B/iteration of quarantined frees over 5000
#     iterations, not a leak) and the check fails for a reason that has nothing
#     to do with the collector.  See
#     docs/archive/history/gc-leak-gate-darwin-sanitized-probe-drift.md, and the
#     two Darwin heap reports archived before it for the same probe-vs-allocator
#     family.
#
# So a probe-output fixture is here for its ASan-clean checks only.  Its output
# assertion lives in tests/run.sh, which compiles fixtures WITHOUT sanitizers --
# where the probe measures what it claims to.  The skip is printed, not silent.
#
# 2026-08-01: `gc-collects-strong-cycle` now reads the CG6 (gc-live-blocks)
# counter instead of a malloc probe, so it MOVED to CONTROL_FIXTURES -- both of
# its skips became real assertions.  The counter is program-scoped and ASan
# cannot perturb it, so the sanitized numbers discriminate: 0 with the collector
# on, 10000 with it off.  The malloc-probe version survives as the
# `-heap-bytes` sibling below, which keeps the two skips because bytes still
# catch a payload leak the block counter cannot see.
PROBE_OUTPUT_FIXTURES="gc-collects-strong-cycle-heap-bytes"
CONTROL_FIXTURES="exg5-exists-cycle gc-stats-observability rcvec-cycle-is-collected gc-collects-strong-cycle"

# TUR_CC_FLAGS REPLACES the default compiler flags rather than appending, so the
# defaults are restated here -- dropping -std=c99 alone is enough to fail the
# link with an undefined `tur_set_contract_handler`.
CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -g"

# Build one fixture variant and run it under ASan.
# $1 = fixture dir, $2 = "on"|"off", $3 = binary path.
# Writes stdout to $WORK/out.$2 and echoes a status word.
build_and_run() {
    local dir="$1" mode="$2" exe="$3"
    local src="$WORK/$(basename "$dir")-$mode.tur"

    if [ "$mode" = "off" ]; then
        # Neutralise the collector at the source: there is no runtime switch
        # that disables it in an already-built program, and turning the
        # fixture's own (gc-enable!) into (gc-disable!) makes every later
        # (gc!) inert, which is precisely the control we want.
        sed 's/(gc-enable!)/(gc-disable!)/' "$dir/input.tur" > "$src"
    else
        cp "$dir/input.tur" "$src"
    fi

    if ! TUR_CC_FLAGS="$CC_FLAGS" "$TUR" build "$src" -o "$exe" \
            > "$WORK/build.log" 2>&1; then
        echo "BUILD-FAILED"
        return
    fi
    # detect_leaks=0: see the header -- LSan cannot see registry-pinned blocks,
    # so leave it off rather than imply an assertion it is not making. ASan's
    # memory-error checking is what this run is for.
    if ASAN_OPTIONS=detect_leaks=0 "$exe" > "$WORK/out.$mode" 2> "$WORK/run.err"; then
        echo "OK"
    elif grep -q "AddressSanitizer" "$WORK/run.err"; then
        echo "ASAN-ERROR"
    else
        echo "RUN-FAILED"
    fi
}

for f in $PROBE_OUTPUT_FIXTURES $CONTROL_FIXTURES; do
    dir="tests/fixtures/$f"
    [ -f "$dir/input.tur" ] || { fail "$f-present" "no input.tur"; continue; }

    on=$(build_and_run "$dir" "on" "$WORK/$f-on")
    case "$on" in
        OK)           pass "$f-asan-clean-collector-on" ;;
        ASAN-ERROR)   fail "$f-asan-clean-collector-on" "ASan reported an error: $(grep -m1 ERROR "$WORK/run.err")" ;;
        BUILD-FAILED) fail "$f-asan-clean-collector-on" "sanitized build failed: $(tail -3 "$WORK/build.log")" ;;
        *)            fail "$f-asan-clean-collector-on" "run failed: $(tail -3 "$WORK/run.err")" ;;
    esac
    on_out=$(cat "$WORK/out.on" 2>/dev/null)

    case " $PROBE_OUTPUT_FIXTURES " in *" $f "*) probe_output=1 ;; *) probe_output=0 ;; esac

    if [ "$probe_output" = "1" ]; then
        skip "$f-collector-on-output-matches" \
             "malloc-probe output is not meaningful under ASan (blind on glibc, quarantine-inflated on Darwin); tests/run.sh asserts it unsanitized"
    elif [ "$on" = "OK" ]; then
        if [ -f "$dir/expected.stdout" ] && [ "$on_out" = "$(cat "$dir/expected.stdout")" ]; then
            pass "$f-collector-on-output-matches"
        else
            fail "$f-collector-on-output-matches" "sanitized output differs from expected.stdout"
        fi
    fi

    off=$(build_and_run "$dir" "off" "$WORK/$f-off")
    case "$off" in
        OK)           pass "$f-asan-clean-collector-off" ;;
        ASAN-ERROR)   fail "$f-asan-clean-collector-off" "ASan reported an error: $(grep -m1 ERROR "$WORK/run.err")" ;;
        BUILD-FAILED) fail "$f-asan-clean-collector-off" "sanitized build failed: $(tail -3 "$WORK/build.log")" ;;
        *)            fail "$f-asan-clean-collector-off" "run failed: $(tail -3 "$WORK/run.err")" ;;
    esac
    off_out=$(cat "$WORK/out.off" 2>/dev/null)

    # The control: with the collector off the program must report retention the
    # on-run did not. Identical output would mean the collector changed nothing
    # and the on-run's success was proving something else.
    case " $CONTROL_FIXTURES " in *" $f "*) run_control=1 ;; *) run_control=0 ;; esac
    if [ "$run_control" = "0" ]; then
        skip "$f-collector-is-what-reclaims" \
             "output comes from a malloc probe, which under ASan reports the sanitizer's allocator rather than the collector's work (identical on/off on glibc; quarantine-inflated on both sides on Darwin)"
    fi
    if [ "$run_control" = "1" ] && [ "$on" = "OK" ] && [ "$off" = "OK" ]; then
        if [ "$on_out" != "$off_out" ]; then
            pass "$f-collector-is-what-reclaims"
        else
            fail "$f-collector-is-what-reclaims" \
                 "identical output with the collector on and off ($on_out)"
        fi
    fi
done

echo
echo "gc-leak-gate: $PASS passed, $SKIP skipped, $FAIL failed"
[ "$FAIL" -eq 0 ]
