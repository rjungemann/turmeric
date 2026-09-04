#!/usr/bin/env bash
#
# run-debugger.sh -- debugger Phase 2 regression test.
#
# Drives `tur debug` (the interactive interpreter debugger) over the
# tests/fixtures/debugger/step-break fixture from a scripted command stream and
# asserts the salient output: program-entry stop, line breakpoints, step /
# next / finish, backtrace, locals, print, the (break) builtin, and continue.
#
# See docs/archive/history/debugger-plan.md (Phase 2) and
# docs/artifacts/debugger-interpreter-phase2.md.
#
# The interpreter intentionally never frees its process-lifetime closures, so
# the harness runs with LeakSanitizer off (matching run-turi.sh / run-flags.sh).

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL debugger: tur binary not found at $TUR (build first)"
  exit 1
fi

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

FIX="tests/fixtures/debugger/input.tur"
if [ ! -f "$FIX" ]; then
  echo "FAIL debugger: fixture not found at $FIX"
  exit 1
fi

pass=0
fail=0

# run <commands> -> echoes the debugger's stdout
run_dbg() {
  printf '%s' "$1" | "$TUR" debug "$FIX" 2>/dev/null
}

# expect <desc> <output> <needle...> -- every needle must appear (substring).
expect() {
  local desc="$1"; shift
  local out="$1"; shift
  local ok=1 missing=""
  for needle in "$@"; do
    if ! grep -qF -- "$needle" <<< "$out"; then
      ok=0; missing="$missing
    missing: $needle"
    fi
  done
  if [ "$ok" -eq 1 ]; then
    echo "PASS debugger: $desc"
    pass=$((pass + 1))
  else
    echo "FAIL debugger: $desc$missing"
    echo "---- actual ----"
    printf '%s\n' "$out" | sed 's/^/    /'
    echo "----------------"
    fail=$((fail + 1))
  fi
}

# refute <desc> <output> <needle> -- needle must NOT appear.
refute() {
  local desc="$1" out="$2" needle="$3"
  if grep -qF -- "$needle" <<< "$out"; then
    echo "FAIL debugger: $desc (unexpected: $needle)"
    echo "---- actual ----"
    printf '%s\n' "$out" | sed 's/^/    /'
    echo "----------------"
    fail=$((fail + 1))
  else
    echo "PASS debugger: $desc"
    pass=$((pass + 1))
  fi
}

# 1) Entry stop + line breakpoint + backtrace + locals + print.
out=$(run_dbg $'break 7\ncontinue\nbacktrace\nlocals\nprint a\ncontinue\n')
expect "entry stop is reported"            "$out" "stopped at input.tur:11"
expect "breakpoint set is acknowledged"    "$out" "breakpoint 1 set at 7"
expect "breakpoint pauses inside add"      "$out" "stopped at input.tur:7"
expect "backtrace shows add over main"     "$out" "#0  add  at input.tur:7" "#1  main  at input.tur:13"
expect "locals show both params"           "$out" "a = 3" "b = 4"
expect "print resolves a single binding"   "$out" "a = 3"
expect "program runs to completion"        "$out" "done"

# 2) step descends into the callee (reaches add's body line 7).
out=$(run_dbg $'step\nstep\nstep\nstep\ncontinue\n')
expect "step reaches the call site"        "$out" "stopped at input.tur:13"
expect "step descends into add's body"     "$out" "stopped at input.tur:7"

# 3) next steps OVER the call: add's body line 7 is never shown.
out=$(run_dbg $'next\nnext\nnext\nnext\ncontinue\n')
expect "next reaches the (break)/println"  "$out" "stopped at input.tur:15"
refute "next does not enter add's body"    "$out" "stopped at input.tur:7"

# 4) finish runs out of add back into the caller: the post-finish backtrace
#    is just main (the add activation has returned).
out=$(run_dbg $'break 7\ncontinue\nfinish\nbacktrace\ncontinue\n')
expect "finish returns into main"          "$out" "stopped at input.tur:11" "#0  main  at input.tur:11"
refute "finish leaves add's frame behind"  "$out" "add  at input.tur"

# 5) the (break) builtin forces a pause even with no breakpoint set.
out=$(run_dbg $'continue\nlist\ncontinue\n')
expect "(break) builtin pauses at line 15" "$out" "stopped at input.tur:15"
expect "list prints a source window"       "$out" "=> 15"

# 6) quit aborts the run.
out=$(run_dbg $'quit\n')
expect "quit aborts the program"           "$out" "aborting"
refute "quit suppresses program output"    "$out" "done"

echo
if [ "$fail" -ne 0 ]; then
  echo "FAIL debugger: $fail assertion(s) failed ($pass passed)"
  exit 1
fi
echo "PASS debugger: all $pass assertions passed"
