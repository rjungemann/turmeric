#!/usr/bin/env bash
#
# run-trace.sh -- the time-travel recorder (editor-intelligence follow-through,
# track T1).
#
# Records a fixture with `tur trace`, then reads the recording back with
# `tur trace --dump` and asserts the shape of what came out: the step and frame
# counts, that a delta only carries a binding whose rendering changed, that the
# program's own stdout still reaches the terminal AND is interleaved into the
# record stream, and that the step cap truncates loudly rather than silently.
#
# The recorder is an interpreter feature, so the fixture carries
# requires.dedicated-runner and tests/run.sh leaves it here.
#
# The interpreter intentionally never frees its process-lifetime closures, so
# LeakSanitizer is off for these runs (matching run-dap.sh and the ctest
# policy in docs/asan-debug-leaks-plan.md).

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL trace: tur binary not found at $TUR (build first)"
  exit 1
fi

FIX="tests/fixtures/trace/input.tur"
if [ ! -f "$FIX" ]; then
  echo "FAIL trace: fixture not found at $FIX"
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
REC="$WORK/fib.turtrace"

pass=0
fail=0
ok()   { echo "PASS trace: $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL trace: $1 -- $2"; fail=$((fail + 1)); }

expect() {  # desc, needle, haystack
  if grep -qF -- "$2" <<< "$3"; then ok "$1"; else bad "$1" "missing '$2'"; fi
}
reject() {
  if grep -qF -- "$2" <<< "$3"; then bad "$1" "unexpected '$2'"; else ok "$1"; fi
}

# ---------------------------------------------------------------------------
# Record
# ---------------------------------------------------------------------------

rec_out=$("$TUR" trace "$FIX" -o "$REC" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
  echo "FAIL trace: recording exited $rc"
  echo "$rec_out"
  exit 1
fi

# The recorder is not a muzzle: `(println (fib 6))` still prints 8.
expect "the program's own output still reaches stdout" "8" "$rec_out"
expect "the summary reports the step count"    "65 steps"     "$rec_out"
expect "the summary reports frame entries"     "26 enters"    "$rec_out"
expect "the summary reports frame exits"       "25 pops"      "$rec_out"
expect "the summary reports the peak depth"    "peak depth 7" "$rec_out"
expect "an untruncated run says so"            "truncated no" "$rec_out"

if [ ! -s "$REC" ]; then
  echo "FAIL trace: -o produced no file"
  exit 1
fi
ok "-o writes the recording"

# ---------------------------------------------------------------------------
# Read it back
#
# The format is exercised by a reader before any UI depends on it, which is the
# ordering the plan asks for: if the format is wrong, a timeline widget is the
# expensive place to find that out.
# ---------------------------------------------------------------------------

dump=$("$TUR" trace --dump "$REC" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
  echo "FAIL trace: --dump exited $rc"
  echo "$dump"
  exit 1
fi

expect "the header round-trips"          "turtrace v1"  "$dump"
expect "frame entries decode"            "ENTER  depth=1" "$dump"
expect "frame exits decode"              "POP    depth=" "$dump"
expect "sites resolve to file and line"  "input.tur:9:3" "$dump"
expect "sites resolve to a function name" "fib"          "$dump"
expect "program output is interleaved"   "OUTPUT 2 bytes" "$dump"

# Deltas, not states: `n` is carried on the step that binds it and on no
# later step of the same frame, because its rendering has not moved.
expect "a binding change is recorded"    "n=6"          "$dump"
n_hits=$(grep -c "n=6" <<< "$dump")
if [ "$n_hits" -eq 1 ]; then
  ok "an unchanged binding is not repeated on every step"
else
  bad "an unchanged binding is not repeated on every step" \
      "n=6 appears $n_hits times, expected 1"
fi

# ---------------------------------------------------------------------------
# The step cap
#
# A recording of a runaway loop is a tab that dies, so the cap is not optional
# -- and truncation is reported, never silent.
# ---------------------------------------------------------------------------

cap_out=$("$TUR" trace "$FIX" --max-steps=10 2>&1)
expect "the cap stops the recording where it says"  "10 steps"      "$cap_out"
expect "a truncated run says so"                    "truncated yes" "$cap_out"
reject "and does not also claim to be complete"     "truncated no"  "$cap_out"

# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------

junk="$WORK/not-a-trace"
printf 'this is not a recording' > "$junk"
junk_out=$("$TUR" trace --dump "$junk" 2>&1)
if [ $? -eq 0 ]; then
  bad "a non-trace file is refused" "--dump exited 0"
else
  expect "a non-trace file is refused with a reason" \
         "not a readable .turtrace" "$junk_out"
fi

echo
echo "trace: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
