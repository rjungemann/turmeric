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

replay_out=$(python3 tests/dap-replay-driver.py "$TUR" "$FIX" 2>&1)
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
  expect_replay "and lands in the caller"         "FRAME back #0 main :18"
  expect_replay "with the caller's own locals"    "VAR back x=3"
  expect_replay "and not the callee's"            "DEPTH after-stepback=1"
  # A recording is replayable: the second visit reads exactly like the first.
  expect_replay "the same breakpoint again"       "DEPTH again=2"
  expect_replay "with the same values"            "VAR again a=3"
  expect_replay "reverseContinue rewinds"         "STOP rev reason="
  expect_replay "the replay session completed"    "DONE"
fi

echo
if [ "$fail" -ne 0 ]; then
  echo "FAIL dap: $fail assertion(s) failed ($pass passed)"
  echo "---- transcript ----"
  printf '%s\n' "$out" | sed 's/^/    /'
  echo "--------------------"
  exit 1
fi
echo "PASS dap: all $pass assertions passed"
