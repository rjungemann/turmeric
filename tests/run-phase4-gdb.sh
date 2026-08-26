#!/usr/bin/env bash
#
# run-phase4-gdb.sh -- debugger Phase 4 regression test (native source maps).
#
# Two halves, both keyed on the same fixture:
#
#   1. `#line` emission: `tur emit-c` is unchanged (no directives), while
#      `tur --debug emit-c` threads `#line N "input.tur"` directives mapping the
#      generated C back to the turmeric source.
#
#   2. Native backtrace: `tur --debug build` compiles the fixture with `-g -Og`
#      and the `#line` directives, so a gdb backtrace of the deliberate crash
#      names turmeric-source frames (input.tur:<line>) and turmeric symbols
#      (boom / middle / main) rather than the C carrier.
#
# The gdb half is skipped (PASS) when gdb is unavailable, mirroring how the DAP
# regression skips without python3. The fixture carries requires.dedicated-runner
# so tests/run.sh leaves it to this harness.
#
# See docs/archive/history/debugger-plan.md (Phase 4) and
# docs/artifacts/debugger-native-sourcemaps-phase4.md.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL phase4: tur binary not found at $TUR (build first)"
  exit 1
fi

FIX="tests/fixtures/debugger-phase4/input.tur"
if [ ! -f "$FIX" ]; then
  echo "FAIL phase4: fixture not found at $FIX"
  exit 1
fi

pass=0
fail=0

# expect <desc> <output> <needle...> -- every needle must appear (substring).
# Uses a here-string (not `printf | grep`): under `pipefail`, grep -q closing a
# large pipe early SIGPIPEs the writer and fails the pipeline, masking a match.
expect() {
  local desc="$1"; shift
  local out="$1"; shift
  local ok=1 missing=""
  for needle in "$@"; do
    if ! grep -qF -- "$needle" <<<"$out"; then
      ok=0; missing="$missing
    missing: $needle"
    fi
  done
  if [ "$ok" -eq 1 ]; then
    echo "PASS phase4: $desc"
    pass=$((pass + 1))
  else
    echo "FAIL phase4: $desc$missing"
    echo "---- actual ----"
    printf '%s\n' "$out" | sed 's/^/    /'
    echo "----------------"
    fail=$((fail + 1))
  fi
}

# refute <desc> <output> <needle> -- needle must NOT appear.
refute() {
  local desc="$1" out="$2" needle="$3"
  if grep -qF -- "$needle" <<<"$out"; then
    echo "FAIL phase4: $desc (unexpected: $needle)"
    fail=$((fail + 1))
  else
    echo "PASS phase4: $desc"
    pass=$((pass + 1))
  fi
}

# -- 1) #line emission is gated on --debug -----------------------------------
plain=$("$TUR" emit-c "$FIX" 2>/dev/null)
refute "default emit-c carries no #line directives" "$plain" "#line "

dbg=$("$TUR" --debug emit-c "$FIX" 2>/dev/null)
expect "--debug emit-c maps the body back to input.tur" "$dbg" \
  '#line ' 'input.tur"'

# -- 2) native backtrace under gdb -------------------------------------------
if ! command -v gdb >/dev/null 2>&1; then
  # PARTIAL, not a whole-suite skip: the emit-c assertions above already ran,
  # so this suite still has a meaningful duration. TUR_SKIP_PARTIAL keeps the
  # timing ingest recording it as a pass with a note, rather than dropping it
  # from the trend the way a bare TUR_SKIP would.
  echo "SKIP phase4: gdb not available -- skipping native backtrace check"
  echo "TUR_SKIP_PARTIAL: gdb unavailable (native backtrace check)"
else
  BIN="$(mktemp -u "${TMPDIR:-/tmp}/tur-phase4-XXXXXX")"
  build_out=$("$TUR" --debug build "$FIX" -o "$BIN" 2>&1)
  if [ ! -x "$BIN" ]; then
    echo "FAIL phase4: --debug build did not produce a binary"
    echo "---- build output ----"
    printf '%s\n' "$build_out" | sed 's/^/    /'
    echo "----------------------"
    fail=$((fail + 1))
  else
    bt=$(gdb -batch -nx -ex run -ex bt "$BIN" 2>&1)
    expect "gdb stops on the deliberate crash"          "$bt" "SIGABRT"
    expect "backtrace names turmeric source + frames"   "$bt" \
      "input.tur:" "boom" "middle" "main"
    refute "backtrace does not surface the .c carrier"  "$bt" "input.tur.c:"
  fi
  rm -f "$BIN"
fi

echo
if [ "$fail" -ne 0 ]; then
  echo "FAIL phase4: $fail assertion(s) failed ($pass passed)"
  exit 1
fi
echo "PASS phase4: all $pass assertions passed"
