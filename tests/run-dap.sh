#!/usr/bin/env bash
#
# run-dap.sh -- debugger Phase 3 regression test.
#
# Drives `tur dap` (the Debug Adapter Protocol server over the interpreter)
# through a scripted JSON-RPC session (tests/dap-driver.py) over a fixture and
# asserts the salient transcript: the initialize/launch/configurationDone
# handshake, breakpoint verification (plain + conditional), the program-entry
# stop, step / next / stepOut, the call stack, locals + evaluate, a conditional
# breakpoint that fires only when its predicate holds, program stdout forwarded
# as an output event, and the exit code.
#
# See docs/archive/history/debugger-plan.md (Phase 3) and
# docs/artifacts/debugger-dap-phase3.md.
#
# The interpreter intentionally never frees its process-lifetime closures, so
# the driver runs the server with LeakSanitizer off (it sets ASAN_OPTIONS).

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL dap: tur binary not found at $TUR (build first)"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP dap: python3 not available -- skipping DAP regression"
  echo "TUR_SKIP: python3 unavailable"
  exit 0
fi

FIX="tests/fixtures/dap/input.tur"
if [ ! -f "$FIX" ]; then
  echo "FAIL dap: fixture not found at $FIX"
  exit 1
fi

out=$(python3 tests/dap-driver.py "$TUR" "$FIX" 2>&1)
status=$?

pass=0
fail=0

expect() {
  local desc="$1" needle="$2"
  if grep -qF -- "$needle" <<< "$out"; then
    echo "PASS dap: $desc"
    pass=$((pass + 1))
  else
    echo "FAIL dap: $desc (missing: $needle)"
    fail=$((fail + 1))
  fi
}

refute() {
  local desc="$1" needle="$2"
  if grep -qF -- "$needle" <<< "$out"; then
    echo "FAIL dap: $desc (unexpected: $needle)"
    fail=$((fail + 1))
  else
    echo "PASS dap: $desc"
    pass=$((pass + 1))
  fi
}

if [ "$status" -ne 0 ]; then
  echo "FAIL dap: driver exited non-zero ($status)"
  echo "---- transcript ----"
  printf '%s\n' "$out" | sed 's/^/    /'
  echo "--------------------"
  exit 1
fi

# Handshake + breakpoint verification.
expect "plain breakpoint verified"        "BPSET line=7 verified=true"
expect "conditional breakpoint verified"  "BPSET line=13 verified=true"
# Program-entry stop + stack.
expect "entry stop reported"              "STOP entry reason=entry"
expect "entry frame is main"              "FRAME entry #0 main input.tur:16"
# next steps over a source line in main.
expect "next stops on the next line"      "STOP next1 reason=step"
expect "next stays in main"               "FRAME next1 #0 main input.tur:17"
# Breakpoint inside add(): two-frame stack, locals, evaluate.
expect "breakpoint pauses inside add"     "STOP bp-add reason=breakpoint"
expect "stack shows add over main"        "FRAME bp-add #0 add input.tur:7"
expect "stack shows the main caller"      "FRAME bp-add #1 main input.tur:18"
expect "locals expose param a"            "VAR add a=3"
expect "locals expose param b"            "VAR add b=4"
expect "evaluate resolves a"              "EVAL a=3"
# stepOut returns to the caller.
expect "stepOut returns to main"          "STOP out reason=step"
expect "post-stepOut frame is main"       "FRAME out #0 main input.tur:19"
# Conditional breakpoint fires ONLY when i == 3 (not at the earlier i=5/4).
expect "conditional breakpoint fires"     "STOP cond reason=breakpoint"
expect "conditional fires at i==3"        "VAR cond i=3"
expect "evaluate resolves i"              "EVAL i=3"
# Program output forwarded + clean exit.
# The timeline extension needs a recording, and refuses with the reason rather
# than the generic "not supported while paused".
expect "live session refuses replayInfo"  "LIVE replayInfo success=false"
expect "and says to relaunch with replay" "relaunch with \"replay\": true"
expect "live session refuses replaySeek"  "LIVE replaySeek success=false"
# Program output forwarded + clean exit.
expect "program stdout as output event"   "OUTPUT done"
expect "exit code is forwarded"           "EXIT code=7"
expect "driver completed the session"     "DONE"

# ---------------------------------------------------------------------------
# T2: reverse execution over a recording
#
# A second session, launched with `"replay": true`. The server records the
# whole run and then serves the session from the recording, which is what makes
# the direction a live debugger cannot go answerable at all.
# ---------------------------------------------------------------------------

# Its own fixture, and a larger one: see tests/fixtures/dap-replay/input.tur
# for why a 65-step recording cannot tell a linear scan from a quadratic one.
REPLAY_FIX="tests/fixtures/dap-replay/input.tur"
if [ ! -f "$REPLAY_FIX" ]; then
  echo "FAIL dap: replay fixture not found at $REPLAY_FIX"
  exit 1
fi
replay_out=$(python3 tests/dap-replay-driver.py "$TUR" "$REPLAY_FIX" 2>&1)
replay_status=$?

expect_replay() {
  local desc="$1" needle="$2"
  if grep -qF -- "$needle" <<< "$replay_out"; then
    echo "PASS dap: $desc"
    pass=$((pass + 1))
  else
    echo "FAIL dap: $desc (missing: $needle)"
    fail=$((fail + 1))
  fi
}

if [ "$replay_status" -ne 0 ]; then
  echo "FAIL dap: replay driver exited non-zero ($replay_status)"
  printf '%s\n' "$replay_out" | sed 's/^/    /'
  fail=$((fail + 1))
else
  expect_replay "stepBack is advertised"        "CAP supportsStepBack=true"
  expect_replay "reverseContinue is advertised" "CAP supportsReverseContinue=true"
  expect_replay "a recording opens at its first step" "STOP entry reason=entry"
  # Forward through the recording behaves like a live session: the same
  # breakpoint, the same two-frame stack, the same locals.
  expect_replay "a recorded breakpoint stops"   "STOP bp-add reason=breakpoint"
  expect_replay "the recorded stack has both frames" "DEPTH bp-add=2"
  expect_replay "recorded locals expose param a" "VAR add a=3"
  expect_replay "recorded locals expose param b" "VAR add b=4"
  # The one request a recording cannot answer says so, rather than returning a
  # stale value that looks like an answer.
  expect_replay "evaluate refuses in a recording" "EVAL success=false"
  expect_replay "and names the reason"            "cannot evaluate in a recording"
  # The gate: stepping BACK out of the callee lands in the caller, showing the
  # caller's own values -- "how did this come to be what it is", answered.
  expect_replay "stepBack leaves the callee"      "STEPBACK left-callee-after=yes"
  expect_replay "and lands in the caller"         "FRAME back #0 main"
  expect_replay "with the caller's own locals"    "VAR back x=3"
  expect_replay "and not the callee's"            "DEPTH after-stepback=1"
  # A recording is replayable: the second visit reads exactly like the first.
  expect_replay "the same breakpoint again"       "DEPTH again=2"
  expect_replay "with the same values"            "VAR again a=3"
  expect_replay "reverseContinue rewinds"         "STOP rev reason="
  # A `continue` with nothing ahead of it scans the whole recording. Reaching
  # `exited` is the assertion; the timing line is the diagnostic that says
  # WHY, if a future change makes that scan quadratic again.
  # T4-T6: the timeline extension. A recording is an axis, and these three
  # requests are what give a scrubber and a depth ribbon something to stand on.
  expect_replay "the timeline extension is advertised" \
      "CAP supportsTurmericReplayTimeline=true"
  expect_replay "replayInfo reports a length"     "INFO steps-positive=yes"
  expect_replay "replaySites honours its bucket count"  "SITES bucketed-len=16"
  # Max-per-bucket, not sample-per-bucket: the two-frame stack the forward pass
  # walked through must survive the downsample.
  expect_replay "the ribbon keeps the deepest call" "SITES peak-at-least-2=yes"
  # Position and depth arrive together -- Try Turmeric's trace-site-at shape.
  # A depths-only reply would make a scrubber ask twice for the same steps.
  expect_replay "a site carries its line"           "SITES carry-position=yes"
  expect_replay "and its file"                      "SITES carry-file=yes"
  # A bucket points at the step its maximum came from, so a ribbon spike is
  # clickable and lands where the deep call actually is.
  expect_replay "a bucket points at its own peak"   "SITES peak-index-is-the-peak=yes"
  # The other way to ask: specific steps, for a cursor readout or a tooltip.
  expect_replay "explicit indices are answered"     "SITES explicit-len=3"
  expect_replay "and echoed back in order"          "SITES explicit-indices-echo=yes"
  expect_replay "and there is a default width"      "SITES default-len=256"
  # A seek reaches a step no amount of stepping would find in reasonable time.
  expect_replay "replaySeek reports where it landed" "SEEK end-index-matches=yes"
  expect_replay "and the cursor actually moved"      "SEEK cursor-followed=yes"
  expect_replay "an out-of-range seek clamps"        "SEEK clamps-high=yes"
  # The last step's transcript is the whole recording's. The fixture's only
  # println is its final act and drains after the final STEP, so a
  # cursor-relative answer reports nothing -- and an empty console at the end
  # of a run that printed reads as a broken timeline.
  expect_replay "the final println is visible"       "SEEK final-println-visible=yes"
  expect_replay "and the end of the run has printed" "SEEK output-at-end=yes"
  expect_replay "seeking back rewinds the transcript" "REWIND transcript-emptied=yes"
  expect_replay "to nothing at the first step"       "REWIND replayOutput-length=0"
  expect_replay "with the cursor back at the start"  "REWIND cursor-at-start=yes"
  expect_replay "and the two agreeing"               "REWIND output-length-agrees=yes"
  expect_replay "continue runs off the end of the recording" "EXIT code=7"
  expect_replay "and the end-to-end scan stays linear" "CONTINUE-TO-END under-10s=yes"
  expect_replay "the replay session completed"    "DONE"
fi

echo
# ---------------------------------------------------------------------------
# debugger-and-tracer-only-instrument-main: a TOP-LEVEL program (no `main`)
# is debuggable too -- stopOnEntry stops on its first form, a breakpoint in a
# function the top level calls fires, output and exit still arrive.  It used
# to run to completion with no `stopped` event at all.
# ---------------------------------------------------------------------------
TLFIX="tests/fixtures/dap-toplevel/input.tur"
tl_out=$(python3 tests/dap-toplevel-driver.py "$TUR" "$TLFIX" 2>&1)
dap_main_out="$out"
out="$tl_out"
expect "top-level: breakpoint verified"        "BPSET line=6 verified=true"
expect "top-level: entry stop reported"        "STOP entry reason=entry"
expect "top-level: entry frame is the top level" "FRAME entry #0 <top> input.tur:3"
expect "top-level: breakpoint fires in callee" "STOP bp reason=breakpoint"
expect "top-level: callee frame"               "FRAME bp #0 use-ask input.tur:6"
expect "top-level: output still arrives"       "OUTPUT 42"
expect "top-level: exit code"                  "EXIT code=0"
out="$dap_main_out"

if [ "$fail" -ne 0 ]; then
  echo "FAIL dap: $fail assertion(s) failed ($pass passed)"
  echo "---- transcript ----"
  printf '%s\n' "$out" | sed 's/^/    /'
  echo "--------------------"
  exit 1
fi
echo "PASS dap: all $pass assertions passed"
