#!/usr/bin/env bash
#
# run-phase5-gdb.sh -- debugger Phase 5 regression (rich type display via gdb
# pretty-printers).  Two halves keyed on the same fixture:
#
#   N1 (type names): `tur --debug build` produces a binary whose DWARF carries
#       the deterministic by-value carrier type names a pretty-printer
#       dispatches on -- Option__int{is_some,value} and Result{is_ok,ok_val,
#       err_val}.  Asserted both in the emitted C (gdb-independent) and, when
#       gdb is present, via `ptype` against the binary's DWARF.
#
#   N2 (pretty-printers): with tools/debug/turmeric_gdb.py sourced, gdb renders
#       a Result parameter as `(ok 14)` and the returned Option as `(some 42)`,
#       not the raw C carrier.
#
# The gdb half is skipped (PASS) when gdb is unavailable, mirroring the Phase 4
# harness.  The fixture carries requires.dedicated-runner so tests/run.sh leaves
# it to this harness.
#
# See docs/archive/history/debugger-plan.md (Phase 5) and
# docs/archive/history/debugger-native-types-plan.md.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
PP="tools/debug/turmeric_gdb.py"
FIX="tests/fixtures/debugger-phase5/input.tur"

if [ ! -x "$TUR" ]; then
  echo "FAIL phase5: tur binary not found at $TUR (build first)"; exit 1
fi
if [ ! -f "$FIX" ]; then
  echo "FAIL phase5: fixture not found at $FIX"; exit 1
fi
if [ ! -f "$PP" ]; then
  echo "FAIL phase5: pretty-printer not found at $PP"; exit 1
fi

pass=0
fail=0

# expect <desc> <output> <needle...> -- every needle must appear (substring).
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
    echo "PASS phase5: $desc"; pass=$((pass + 1))
  else
    echo "FAIL phase5: $desc$missing"
    echo "---- actual ----"; printf '%s\n' "$out" | sed 's/^/    /'; echo "----------------"
    fail=$((fail + 1))
  fi
}

# -- N1a) emit-c carries the discoverable carrier type names (no gdb needed) --
emitted=$("$TUR" emit-c "$FIX" 2>/dev/null)
expect "emit-c names the by-value Option carrier" "$emitted" \
  "tur_adt_Option__int" "bool is_some" "int64_t value"
expect "emit-c names the by-value Result carrier" "$emitted" \
  "tur_adt_Result__int__cstr" "bool is_ok" "int64_t ok_val" "const char * err_val"

# -- build the debug binary (shared by N1b + N2) -----------------------------
BIN="$(mktemp -u "${TMPDIR:-/tmp}/tur-phase5-XXXXXX")"
build_out=$("$TUR" --debug build "$FIX" -o "$BIN" 2>&1)
if [ ! -x "$BIN" ]; then
  echo "FAIL phase5: --debug build did not produce a binary"
  echo "---- build output ----"; printf '%s\n' "$build_out" | sed 's/^/    /'; echo "----------------------"
  echo; echo "FAIL phase5: $((fail + 1)) assertion(s) failed ($pass passed)"; exit 1
fi

if ! command -v gdb >/dev/null 2>&1; then
  echo "PASS phase5: gdb not available -- skipping DWARF + pretty-printer checks"
else
  # -- N1b) the carrier type names round-trip into DWARF ----------------------
  # The Result parameter is the concrete `(Result int cstr)`, which monomorphizes
  # to the by-value struct `tur_adt_Result__int__cstr` (a bare generic `Result`
  # is passed as the int64 carrier and never materializes a struct in DWARF).
  types=$(gdb -batch -nx \
    -ex "ptype tur_adt_Option__int" \
    -ex "ptype tur_adt_Result__int__cstr" "$BIN" 2>&1)
  expect "DWARF carries tur_adt_Option__int{is_some,value}" "$types" \
    "tur_adt_Option__int" "is_some" "value"
  expect "DWARF carries tur_adt_Result__int__cstr{is_ok,ok_val,err_val}" "$types" \
    "tur_adt_Result__int__cstr" "is_ok" "ok_val" "err_val"

  # -- N2) pretty-printers render Turmeric shapes ----------------------------
  SYM=$(gdb -batch -nx -ex "info functions probe" "$BIN" 2>&1 \
        | grep -oE "probe__spec__[A-Za-z0-9_]+" | head -1)
  if [ -z "$SYM" ]; then
    echo "FAIL phase5: could not locate the monomorphized probe symbol in DWARF"
    fail=$((fail + 1))
  else
    pretty=$(gdb -batch -nx \
      -ex "source $PP" \
      -ex "break $SYM" \
      -ex run \
      -ex "print r" \
      -ex "finish" "$BIN" 2>&1)
    expect "gdb renders the Result parameter as (ok 14)"      "$pretty" "(ok 14)"
    expect "gdb renders the returned Option as (some 42)"     "$pretty" "Value returned is" "(some 42)"
  fi
fi

rm -f "$BIN"

echo
if [ "$fail" -ne 0 ]; then
  echo "FAIL phase5: $fail assertion(s) failed ($pass passed)"; exit 1
fi
echo "PASS phase5: all $pass assertions passed"
